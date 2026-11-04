/* SPDX-License-Identifier: Apache-2.0 */
package com.android.server.wm;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;
import android.util.Slog;

import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

import libcore.io.IoUtils;

/** Fixed AOSP15 launch adapter. Exact ActivityRecord tokens bridge pending cold attach. */
final class DaguLaunchProfile {
    private static final String TAG = "DaguLaunchProfile";
    private static final String ENABLED_PROPERTY = "sys.dagu_launch_profile.enabled";
    private static final int WIRE_MAGIC = 0x32504c44;
    private static final int WIRE_VERSION = 2;
    private static final int WIRE_HEADER_SIZE = 72;
    private static final int WIRE_BEGIN_SIZE = 16;
    private static final int WIRE_PATH_SIZE = 8;
    private static final int WIRE_ACK_SIZE = 32;
    private static final int WIRE_MAX_PACKET = 65535;
    private static final int MAX_PACKAGE = 255;
    private static final int MAX_PATHS = 128;
    private static final int MAX_PATH = 4096;
    private static final int MAX_SIGNERS = 16;
    private static final int OP_PREPARE = 1;
    private static final int OP_BEGIN = 2;
    private static final int OP_FINISH = 3;
    private static final int OP_ABORT = 4;
    private static final int OP_INVALIDATE = 5;
    private static final int OP_OFF = 6;
    private static final int PATH_BASE_APK = 1;
    private static final int PATH_SPLIT_APK = 2;
    private static final int PATH_NATIVE_DIR = 3;
    private static final int PATH_SHARED_FILE = 4;
    private static final int PATH_RESOURCE_DIR = 5;
    private static final int INVALIDATE_PACKAGE = 1;
    private static final int INVALIDATE_USER = 2;
    private static final int REASON_UPDATED = 1;
    private static final int REASON_REMOVED = 2;
    private static final int REASON_DATA_CLEARED = 3;
    private static final int REASON_USER_REMOVED = 4;
    private static final int MAX_PENDING_INVALIDATIONS = 256;

    private static native int[] binderIdentity(IBinder binder);
    private static final AtomicLong NEXT_COOKIE = new AtomicLong();
    private final ActivityTaskSupervisor mSupervisor;
    private final Context mContext;
    private final PackageManager mPackageManager;
    private final Handler mWorker;
    private final AtomicLong mEpoch = new AtomicLong(1);
    private final ArrayDeque<Invalidation> mPendingInvalidations = new ArrayDeque<>();
    private Session mActive; // Worker owned, one collector session.
    private LocalSocket mSocket; // Worker only; disconnect revokes controller.
    private boolean mReceiverRegistered;

    private static final class PathInfo {
        final int kind;
        final String path;
        final byte[] utf8;

        PathInfo(int kind, String path) {
            this.kind = kind;
            this.path = path;
            this.utf8 = path.getBytes(StandardCharsets.UTF_8);
        }
    }

    private static final class Metadata {
        final String packageName;
        final int userId;
        final int uid;
        final long longVersionCode;
        final long generation;
        final byte[][] signers;
        final List<PathInfo> paths;

        Metadata(String packageName, int userId, int uid, long longVersionCode,
                byte[][] signers, List<PathInfo> paths) {
            this.packageName = packageName;
            this.userId = userId;
            this.uid = uid;
            this.longVersionCode = longVersionCode;
            this.signers = signers;
            this.paths = paths;
            this.generation = computeGeneration();
        }

        private long computeGeneration() {
            try {
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                ByteBuffer scalar = ByteBuffer.allocate(18).order(ByteOrder.LITTLE_ENDIAN);
                scalar.putInt(userId).putInt(uid).putLong(longVersionCode);
                scalar.putShort((short) packageName.getBytes(StandardCharsets.UTF_8).length);
                digest.update(scalar.array());
                digest.update(packageName.getBytes(StandardCharsets.UTF_8));
                digest.update((byte) signers.length);
                for (byte[] signer : signers) digest.update(signer);
                scalar = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN);
                scalar.putShort((short) paths.size());
                digest.update(scalar.array());
                for (PathInfo path : paths) {
                    digest.update((byte) path.kind);
                    scalar = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN);
                    scalar.putShort((short) path.utf8.length);
                    digest.update(scalar.array());
                    digest.update(path.utf8);
                }
                long value = ByteBuffer.wrap(digest.digest()).getLong();
                return value == 0 ? 1 : value;
            } catch (NoSuchAlgorithmException impossible) {
                throw new AssertionError(impossible);
            }
        }
    }

    private static final class Invalidation {
        final int userId;
        final String packageName;
        final int kind;
        final int reason;

        Invalidation(int userId, String packageName, int kind, int reason) {
            this.userId = userId;
            this.packageName = packageName;
            this.kind = kind;
            this.reason = reason;
        }
    }

    static final class Session {
        final long cookie = NEXT_COOKIE.incrementAndGet();
        final long epoch;
        Session(long epoch) { this.epoch = epoch; }
        volatile ActivityRecord activity; // WM lock only: exact launch token.
        volatile String packageName;
        volatile int userId;
        volatile WindowProcessController process;
        volatile int pid;
        volatile int uid;
        volatile IBinder thread; // Strong until worker finishes ACK/cancellation.
        // 0 pending attach; 1 identity captured; 2 BEGIN in flight; 3 finished; 4 aborted.
        final AtomicInteger state = new AtomicInteger();
        int handle;
        Metadata metadata;
        volatile boolean replayPosted;
        volatile boolean replayPrepared;
        volatile long replayGeneration;
        MiMemEngineDeathRegistration death;
    }

    DaguLaunchProfile(ActivityTaskSupervisor supervisor) {
        mSupervisor = supervisor;
        mContext = supervisor.mService.mContext;
        mPackageManager = mContext.getPackageManager();
        HandlerThread thread = new HandlerThread(TAG);
        thread.start();
        mWorker = new Handler(thread.getLooper());
        mWorker.post(this::registerInvalidationReceiver);
        mWorker.post(this::monitorEnabledProperty);
    }

    Session pending() {
        return new Session(mEpoch.get());
    }

    void associate(Session session, ActivityRecord activity, WindowProcessController expected) {
        if (session == null || session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
        // Coalesced transitions must not reassign a cookie to a different token.
        if (session.activity != null && session.activity != activity) {
            end(session, false);
            return;
        }
        session.activity = activity;
        if (session.process == null) session.process = expected;
        session.packageName = activity.packageName;
        session.userId = activity.mUserId;
        if (!session.replayPosted && session.state.get() < 3) {
            session.replayPosted = true;
            mWorker.post(() -> prepareReplay(session));
        }
        if (activity.app != null) attached(session, activity, activity.app);
    }

    void attached(Session session, ActivityRecord activity, WindowProcessController process) {
        if (session == null || session.activity != activity || activity.app != process
                || (session.process != null && session.process != process)
                || session.epoch != mEpoch.get()
                || process.getThread() == null || process.getPid() <= 0
                || process.mUid != activity.info.applicationInfo.uid
                || !android.os.SystemProperties.getBoolean(ENABLED_PROPERTY, false)
                || !session.state.compareAndSet(0, 1)) return;
        // Called under WM lock by realStartActivityLocked with the exact r/proc pair.
        // AMS already validated the attaching process startSeq and caller identity.
        session.process = process;
        session.pid = process.getPid();
        session.uid = process.mUid;
        session.thread = process.getThread().asBinder();
        session.death = new MiMemEngineDeathRegistration(session.thread);
        mWorker.post(() -> prepare(session));
    }

    void end(Session session, boolean success) {
        if (session == null) return;
        int prior;
        do {
            prior = session.state.get();
            if (prior >= 3) return;
        } while (!session.state.compareAndSet(prior, success ? 3 : 4));
        mWorker.post(() -> complete(session));
    }

    private void prepareReplay(Session session) {
        try {
            if (session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            Metadata metadata = queryMetadata(session.packageName, session.userId);
            if (metadata == null
                    || (session.uid != 0 && metadata.uid != session.uid)
                    || session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            synchronized (mSupervisor.mService.mGlobalLock) {
                if (session.activity == null
                        || !session.packageName.equals(session.activity.packageName)
                        || session.activity.mUserId != session.userId
                        || session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            }
            ensureSocket();
            if (mActive != null && mActive != session) abortActive(true);
            sendMetadata(OP_PREPARE, session, metadata, null);
            session.replayGeneration = metadata.generation;
            session.replayPrepared = true;
            flushInvalidations();
        } catch (IOException | RuntimeException e) {
            Slog.w(TAG, "launch replay prepare failed", e);
            disconnect();
            abortSession(session, false);
        }
    }

    private void prepare(Session session) {
        FileDescriptor binderFd = null;
        try {
            if (session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            Metadata metadata = queryMetadata(session.packageName, session.userId);
            if (metadata == null || metadata.uid != session.uid) {
                abortSession(session, false);
                return;
            }
            synchronized (mSupervisor.mService.mGlobalLock) {
                if (session.process.getPid() != session.pid
                        || session.process.getThread() == null
                        || session.process.getThread().asBinder() != session.thread
                        || session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            }
            // Kernel resolves the exact Binder node owner and verifies PID+UID.
            // Session holds the Binder strongly, preventing handle reuse.
            int[] identity = binderIdentity(session.thread);
            if (identity == null) {
                abortSession(session, false);
                return;
            }
            binderFd = new FileDescriptor();
            binderFd.setInt$(identity[0]);
            session.handle = identity[1];
            synchronized (mSupervisor.mService.mGlobalLock) {
                if (session.process.getPid() != session.pid
                        || session.process.getThread() == null
                        || session.process.getThread().asBinder() != session.thread
                        || session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            }
            session.death.link(() -> end(session, false));
            if (!session.state.compareAndSet(1, 2)) {
                session.death.unlink();
                return;
            }
            ensureSocket();
            if (mActive != null && mActive != session) abortActive(true);
            if (!session.replayPrepared
                    || session.replayGeneration != metadata.generation) {
                sendMetadata(OP_PREPARE, session, metadata, null);
                session.replayGeneration = metadata.generation;
                session.replayPrepared = true;
            }
            session.metadata = metadata;
            sendBegin(session, metadata, binderFd);
            mActive = session;
            Slog.d(TAG, "BEGIN cookie=" + session.cookie + " tid=" + session.pid
                    + " package=" + metadata.packageName + " generation="
                    + metadata.generation);
            // done racing a committed BEGIN is ordered behind it on this worker.
            if (session.state.get() >= 3) {
                complete(session);
                return;
            }
            flushInvalidations();
        } catch (IOException | RemoteException | RuntimeException e) {
            Slog.w(TAG, "launch profile prepare failed", e);
            abortSession(session, true);
        } finally {
            IoUtils.closeQuietly(binderFd);
        }
    }

    private void complete(Session session) {
        try {
            if (mActive != session) {
                if (session.replayPrepared && mSocket != null) {
                    try {
                        sendAbort(session);
                    } catch (IOException ignored) {
                        disconnect();
                    }
                }
                release(session);
                return;
            }
            boolean success = session.state.get() == 3;
            if (success) {
                Metadata refreshed = queryMetadata(session.packageName, session.userId);
                if (refreshed == null || refreshed.generation != session.metadata.generation
                        || refreshed.uid != session.metadata.uid) {
                    sendAbort(session);
                    mActive = null;
                    release(session);
                    return;
                }
                // FINISH carries the refreshed full metadata. The helper rejects a
                // generation change before freezing and queues digest/persistence work.
                sendMetadata(OP_FINISH, session, refreshed, null);
            } else {
                sendAbort(session);
            }
            mActive = null;
            release(session);
        } catch (IOException | RuntimeException e) {
            disconnect();
            release(session);
        }
    }

    private void abortActive(boolean sendPacket) {
        Session old = mActive;
        if (old == null) return;
        old.state.set(4);
        if (sendPacket) {
            try {
                sendAbort(old);
            } catch (IOException ignored) {
                disconnect();
            }
        }
        mActive = null;
        release(old);
    }

    private void abortSession(Session session, boolean sendPacket) {
        session.state.set(4);
        if (mActive == session) mActive = null;
        if (sendPacket && session.replayPrepared && mSocket != null) {
            try {
                sendAbort(session);
            } catch (IOException ignored) {
                disconnect();
            }
        }
        session.replayPrepared = false;
        release(session);
    }

    private Metadata queryMetadata(String packageName, int userId) {
        if (packageName == null) return null;
        try {
            PackageInfo info = mPackageManager.getPackageInfoAsUser(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES, userId);
            ApplicationInfo application = info.applicationInfo;
            if (application == null) return null;
            byte[] packageBytes = packageName.getBytes(StandardCharsets.UTF_8);
            if (packageBytes.length == 0 || packageBytes.length > MAX_PACKAGE) return null;
            byte[][] signers = signerDigests(info.signingInfo);
            if (signers == null || signers.length == 0) return null;
            List<PathInfo> paths = packagePaths(application);
            if (paths == null) return null;
            return new Metadata(packageName, userId, application.uid,
                    info.getLongVersionCode(), signers, paths);
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            Slog.w(TAG, "cannot query package metadata for " + packageName + "/" + userId, e);
            return null;
        }
    }

    private static byte[][] signerDigests(SigningInfo signingInfo) {
        if (signingInfo == null) return null;
        Signature[] current = signingInfo.getApkContentsSigners();
        if (current == null || current.length == 0 || current.length > MAX_SIGNERS) return null;
        List<byte[]> digests = new ArrayList<>(current.length);
        try {
            for (Signature signature : current) {
                digests.add(MessageDigest.getInstance("SHA-256")
                        .digest(signature.toByteArray()));
            }
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
        Collections.sort(digests, DaguLaunchProfile::compareBytes);
        List<byte[]> unique = new ArrayList<>(digests.size());
        for (byte[] digest : digests) {
            if (unique.isEmpty() || !Arrays.equals(unique.get(unique.size() - 1), digest)) {
                unique.add(digest);
            }
        }
        return unique.toArray(new byte[0][]);
    }

    private static int compareBytes(byte[] left, byte[] right) {
        return Arrays.compareUnsigned(left, right);
    }

    private static List<PathInfo> packagePaths(ApplicationInfo application) {
        List<PathInfo> paths = new ArrayList<>();
        if (application.sourceDir == null || application.sourceDir.isEmpty()) return null;
        paths.add(new PathInfo(PATH_BASE_APK, application.sourceDir));
        if (application.splitSourceDirs != null) {
            for (String path : application.splitSourceDirs) addPath(paths, PATH_SPLIT_APK, path);
        }
        addPath(paths, PATH_NATIVE_DIR, application.nativeLibraryDir);
        if (application.sharedLibraryFiles != null) {
            for (String path : application.sharedLibraryFiles) addPath(paths, PATH_SHARED_FILE, path);
        }
        if (application.resourceDirs != null) {
            for (String path : application.resourceDirs) addPath(paths, PATH_RESOURCE_DIR, path);
        }
        paths.sort((left, right) -> {
            if (left.kind != right.kind) return Integer.compare(left.kind, right.kind);
            return left.path.compareTo(right.path);
        });
        List<PathInfo> unique = new ArrayList<>(paths.size());
        Set<String> seen = new HashSet<>();
        for (PathInfo path : paths) {
            String key = path.kind + ":" + path.path;
            if (!seen.add(key)) continue;
            if (!path.path.startsWith("/") || path.utf8.length == 0
                    || path.utf8.length >= MAX_PATH) return null;
            unique.add(path);
        }
        return unique.size() <= MAX_PATHS ? unique : null;
    }

    private static void addPath(List<PathInfo> paths, int kind, String path) {
        if (path != null && !path.isEmpty()) paths.add(new PathInfo(kind, path));
    }

    private void ensureSocket() throws IOException {
        if (mSocket != null) return;
        LocalSocket socket = new LocalSocket(LocalSocket.SOCKET_SEQPACKET);
        socket.connect(new LocalSocketAddress("dagu_launch_profile",
                LocalSocketAddress.Namespace.RESERVED));
        socket.setSoTimeout(200);
        if (socket.getPeerCredentials().getUid() != 0) {
            IoUtils.closeQuietly(socket);
            throw new IOException("untrusted dagu_launch_profile peer");
        }
        mSocket = socket;
    }

    private void sendMetadata(int operation, Session session, Metadata metadata,
            FileDescriptor binderFd) throws IOException {
        int packageLength = metadata.packageName.getBytes(StandardCharsets.UTF_8).length;
        int size = WIRE_HEADER_SIZE + packageLength + metadata.signers.length * 32;
        for (PathInfo path : metadata.paths) size += WIRE_PATH_SIZE + path.utf8.length;
        if (size > WIRE_MAX_PACKET) throw new IOException("metadata packet too large");
        ByteBuffer packet = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        putHeader(packet, operation, 0, session, metadata, packageLength,
                metadata.paths.size(), metadata.signers.length, size);
        packet.put(metadata.packageName.getBytes(StandardCharsets.UTF_8));
        for (byte[] signer : metadata.signers) packet.put(signer);
        for (PathInfo path : metadata.paths) {
            packet.put((byte) path.kind).put((byte) 0).put((byte) 0).put((byte) 0);
            packet.putShort((short) path.utf8.length).putShort((short) 0).put(path.utf8);
        }
        sendPacket(packet, session.cookie, session.epoch, binderFd);
    }

    private void sendBegin(Session session, Metadata metadata, FileDescriptor binderFd)
            throws IOException {
        byte[] packageBytes = metadata.packageName.getBytes(StandardCharsets.UTF_8);
        int size = WIRE_HEADER_SIZE + packageBytes.length + WIRE_BEGIN_SIZE;
        ByteBuffer packet = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        putHeader(packet, OP_BEGIN, 0, session, metadata, packageBytes.length, 0, 0, size);
        packet.put(packageBytes);
        packet.putInt(session.pid).putInt(session.handle).putInt(1000).putInt(0);
        sendPacket(packet, session.cookie, session.epoch, binderFd);
    }

    private void sendAbort(Session session) throws IOException {
        ByteBuffer packet = ByteBuffer.allocate(WIRE_HEADER_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN);
        putHeader(packet, OP_ABORT, 0, session, null, 0, 0, 0, WIRE_HEADER_SIZE);
        sendPacket(packet, session.cookie, session.epoch, null);
    }

    private static void putHeader(ByteBuffer packet, int operation, int flags,
            Session session, Metadata metadata, int packageLength, int pathCount,
            int signerCount, int size) {
        packet.putInt(WIRE_MAGIC).putShort((short) WIRE_VERSION).putShort((short) size);
        packet.putInt(operation).putInt(flags);
        packet.putLong(session.epoch).putLong(session.cookie);
        packet.putInt(session.userId).putInt(metadata == null
                ? session.uid : metadata.uid);
        packet.putLong(metadata == null ? 0 : metadata.generation);
        packet.putLong(metadata == null ? 0 : metadata.longVersionCode);
        packet.putInt(packageLength).putInt(pathCount).putInt(signerCount).putInt(0);
    }

    private void sendPacket(ByteBuffer packet, long cookie, long epoch,
            FileDescriptor descriptor) throws IOException {
        ensureSocket();
        mSocket.setFileDescriptorsForSend(descriptor == null
                ? null : new FileDescriptor[]{descriptor});
        try {
            mSocket.getOutputStream().write(packet.array());
        } finally {
            mSocket.setFileDescriptorsForSend(null);
        }
        byte[] reply = new byte[WIRE_ACK_SIZE];
        if (mSocket.getInputStream().read(reply) != reply.length) {
            throw new IOException("truncated dagu_launch_profile ACK");
        }
        ByteBuffer ack = ByteBuffer.wrap(reply).order(ByteOrder.LITTLE_ENDIAN);
        if (ack.getInt() != WIRE_MAGIC || ack.getShort() != WIRE_VERSION
                || ack.getShort() != WIRE_ACK_SIZE || ack.getInt() != packet.getInt(8)
                || ack.getInt() != 0 || ack.getLong() != epoch
                || ack.getLong() != cookie) {
            throw new IOException("dagu_launch_profile request rejected");
        }
    }

    // Worker only, after ACK or controller revocation; do not retain the strong
    // Binder through ActivityMetricsLogger's longer-lived transition history.
    private void release(Session session) {
        if (session.death != null) session.death.unlink();
        session.death = null;
        session.thread = null;
    }

    private void disconnect() {
        mEpoch.incrementAndGet();
        if (mActive != null) {
            mActive.state.set(4);
            mActive.replayPrepared = false;
            release(mActive);
            mActive = null;
        }
        if (mSocket == null) return;
        try { mSocket.close(); } catch (IOException ignored) { }
        mSocket = null;
    }

    private void registerInvalidationReceiver() {
        if (mReceiverRegistered) return;
        IntentFilter packageFilter = new IntentFilter();
        packageFilter.addAction(Intent.ACTION_PACKAGE_REMOVED);
        packageFilter.addAction(Intent.ACTION_PACKAGE_CHANGED);
        packageFilter.addAction(Intent.ACTION_PACKAGE_REPLACED);
        packageFilter.addAction(Intent.ACTION_PACKAGE_DATA_CLEARED);
        packageFilter.addDataScheme("package");
        IntentFilter userFilter = new IntentFilter(Intent.ACTION_USER_REMOVED);
        try {
            mContext.registerReceiverAsUser(mInvalidationReceiver, UserHandle.ALL,
                    packageFilter, null, mWorker, Context.RECEIVER_NOT_EXPORTED);
            mContext.registerReceiverAsUser(mInvalidationReceiver, UserHandle.ALL,
                    userFilter, null, mWorker, Context.RECEIVER_NOT_EXPORTED);
            mReceiverRegistered = true;
        } catch (RuntimeException e) {
            Slog.w(TAG, "cannot register profile invalidation receiver", e);
        }
    }

    private final BroadcastReceiver mInvalidationReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (Intent.ACTION_USER_REMOVED.equals(action)) {
                UserHandle user = intent.getParcelableExtra(Intent.EXTRA_USER, UserHandle.class);
                if (user != null) {
                    queueInvalidation(new Invalidation(user.getIdentifier(), null,
                            INVALIDATE_USER, REASON_USER_REMOVED));
                }
            } else if (action != null) {
                Uri data = intent.getData();
                String packageName = data == null ? null : data.getSchemeSpecificPart();
                int uid = intent.getIntExtra(Intent.EXTRA_UID, -1);
                UserHandle user = intent.getParcelableExtra(Intent.EXTRA_USER,
                        UserHandle.class);
                int userId = user != null ? user.getIdentifier()
                        : (uid >= 0 ? UserHandle.getUserId(uid) : UserHandle.USER_SYSTEM);
                int reason = Intent.ACTION_PACKAGE_DATA_CLEARED.equals(action)
                        ? REASON_DATA_CLEARED
                        : (Intent.ACTION_PACKAGE_REMOVED.equals(action)
                                ? REASON_REMOVED : REASON_UPDATED);
                if (packageName != null) {
                    queueInvalidation(new Invalidation(userId, packageName,
                            INVALIDATE_PACKAGE, reason));
                }
            }
            flushInvalidations();
        }
    };

    private void queueInvalidation(Invalidation invalidation) {
        if (mPendingInvalidations.size() >= MAX_PENDING_INVALIDATIONS) {
            Slog.w(TAG, "dropping profile invalidation queue overflow");
            return;
        }
        mPendingInvalidations.add(invalidation);
    }

    private void flushInvalidations() {
        if (mPendingInvalidations.isEmpty()
                || !android.os.SystemProperties.getBoolean(ENABLED_PROPERTY, false)) return;
        try {
            ensureSocket();
            while (!mPendingInvalidations.isEmpty()) {
                Invalidation invalidation = mPendingInvalidations.peek();
                if (!sendInvalidation(invalidation)) break;
                mPendingInvalidations.remove();
            }
        } catch (IOException | RuntimeException e) {
            disconnect();
        }
    }

    private void checkEnabledProperty() {
        if (android.os.SystemProperties.getBoolean(ENABLED_PROPERTY, false)) return;
        if (mActive != null) {
            mActive.state.set(4);
            mActive.replayPrepared = false;
            release(mActive);
            mActive = null;
        }
        if (mSocket != null) {
            try {
                sendOff();
            } catch (IOException | RuntimeException e) {
                Slog.w(TAG, "profile OFF acknowledgement unavailable", e);
            }
        }
        disconnect();
    }

    private void monitorEnabledProperty() {
        boolean enabled = android.os.SystemProperties.getBoolean(ENABLED_PROPERTY, false);
        if (!enabled) checkEnabledProperty();
        mWorker.postDelayed(this::monitorEnabledProperty, enabled ? 1000 : 5000);
    }

    private void sendOff() throws IOException {
        long cookie = NEXT_COOKIE.incrementAndGet();
        long epoch = mEpoch.get();
        ByteBuffer packet = ByteBuffer.allocate(WIRE_HEADER_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN);
        packet.putInt(WIRE_MAGIC).putShort((short) WIRE_VERSION)
                .putShort((short) WIRE_HEADER_SIZE).putInt(OP_OFF).putInt(0);
        packet.putLong(epoch).putLong(cookie);
        packet.putInt(0).putInt(0).putLong(0).putLong(0);
        packet.putInt(0).putInt(0).putInt(0).putInt(0);
        sendPacket(packet, cookie, epoch, null);
    }

    private boolean sendInvalidation(Invalidation invalidation) throws IOException {
        byte[] packageBytes = invalidation.packageName == null ? new byte[0]
                : invalidation.packageName.getBytes(StandardCharsets.UTF_8);
        int size = WIRE_HEADER_SIZE + packageBytes.length;
        ByteBuffer packet = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        long cookie = NEXT_COOKIE.incrementAndGet();
        long epoch = mEpoch.get();
        packet.putInt(WIRE_MAGIC).putShort((short) WIRE_VERSION).putShort((short) size);
        packet.putInt(OP_INVALIDATE)
                .putInt(invalidation.kind | (invalidation.reason << 8));
        packet.putLong(epoch).putLong(cookie);
        packet.putInt(invalidation.userId).putInt(0);
        packet.putLong(0).putLong(0);
        packet.putInt(packageBytes.length).putInt(0).putInt(0).putInt(0);
        packet.put(packageBytes);
        sendPacket(packet, cookie, epoch, null);
        return true;
    }
}
