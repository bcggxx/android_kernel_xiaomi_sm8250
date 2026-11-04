/* SPDX-License-Identifier: Apache-2.0 */
package com.android.server.wm;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Slog;

import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

import libcore.io.IoUtils;

/** Fixed AOSP15 launch adapter. Exact ActivityRecord tokens bridge pending cold attach. */
final class DaguLaunchProfile {
    private static native int[] binderIdentity(IBinder binder);
    private static final AtomicLong NEXT_COOKIE = new AtomicLong();
    private final ActivityTaskSupervisor mSupervisor;
    private final Handler mWorker;
    private final AtomicLong mEpoch = new AtomicLong(1);
    private Session mActive; // Worker owned, one collector session.
    private LocalSocket mSocket; // Worker only; disconnect revokes controller.

    static final class Session {
        final long cookie = NEXT_COOKIE.incrementAndGet();
        final long epoch;
        Session(long epoch) { this.epoch = epoch; }
        ActivityRecord activity; // WM lock only: exact launch token, never a PID lookup.
        String packageName;
        int userId;
        WindowProcessController process;
        int pid;
        int uid;
        IBinder thread; // Strong until worker finishes ACK/cancellation.
        // 0 pending attach; 1 identity captured; 2 BEGIN in flight; 3 finished; 4 aborted.
        final AtomicInteger state = new AtomicInteger();
        int handle;
        MiMemEngineDeathRegistration death;
    }

    DaguLaunchProfile(ActivityTaskSupervisor supervisor) {
        mSupervisor = supervisor;
        HandlerThread thread = new HandlerThread("DaguLaunchProfile");
        thread.start();
        mWorker = new Handler(thread.getLooper());
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
        if (activity.app != null) attached(session, activity, activity.app);
    }

    void attached(Session session, ActivityRecord activity, WindowProcessController process) {
        if (session == null || session.activity != activity || activity.app != process
                || (session.process != null && session.process != process)
                || session.epoch != mEpoch.get()
                || process.getThread() == null || process.getPid() <= 0
                || process.mUid != activity.info.applicationInfo.uid
                || !android.os.SystemProperties.getBoolean("sys.dagu_launch_profile.enabled", false)
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
        mWorker.post(() -> {
            try {
                if (mSocket != null && mActive == session) {
                    send(session.state.get() == 3 ? 2 : 3, session, null);
                    mActive = null;
                }
            } catch (IOException e) {
                disconnect();
            }
            release(session);
        });
    }

    private void prepare(Session session) {
        FileDescriptor binderFd = null;
        try {
            if (session.state.get() >= 3 || session.epoch != mEpoch.get()) return;
            // Kernel resolves the exact Binder node owner and verifies PID+UID.
            // Session holds the Binder strongly, preventing handle reuse.
            int[] identity = binderIdentity(session.thread);
            if (identity == null) return;
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
            if (mSocket == null) {
                mSocket = new LocalSocket(LocalSocket.SOCKET_SEQPACKET);
                mSocket.connect(new LocalSocketAddress("dagu_launch_profile",
                        LocalSocketAddress.Namespace.RESERVED));
                mSocket.setSoTimeout(200);
                if (mSocket.getPeerCredentials().getUid() != 0) {
                    throw new IOException("untrusted dagu_launch_profile peer");
                }
            }
            if (mActive != null && mActive != session) {
                Session old = mActive;
                old.state.set(4);
                send(3, old, null);
                release(old);
                mActive = null;
            }
            send(1, session, binderFd);
            mActive = session;
            Slog.d("DaguLaunchProfile", "BEGIN cookie=" + session.cookie
                    + " tid=" + session.pid);
            // done racing a committed BEGIN is ordered behind it on this worker.
            if (session.state.get() >= 3) {
                send(session.state.get() == 3 ? 2 : 3, session, null);
                mActive = null;
            }
        } catch (IOException | RemoteException e) {
            disconnect();
            end(session, false);
        } finally {
            IoUtils.closeQuietly(binderFd);
        }
    }

    private void send(int operation, Session session, FileDescriptor binderFd) throws IOException {
        ByteBuffer packet = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN);
        packet.putInt(1).putInt(operation).putLong(session.cookie);
        packet.putInt(session.uid).putInt(1000).putInt(session.pid).putInt(session.handle);
        mSocket.setFileDescriptorsForSend(binderFd == null ? null : new FileDescriptor[]{binderFd});
        mSocket.getOutputStream().write(packet.array());
        mSocket.setFileDescriptorsForSend(null);
        // Every command receives a bounded status reply. No silent protocol drift.
        byte[] reply = new byte[4];
        if (mSocket.getInputStream().read(reply) != 4
                || ByteBuffer.wrap(reply).order(ByteOrder.LITTLE_ENDIAN).getInt() != 0) {
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
            release(mActive);
            mActive = null;
        }
        if (mSocket == null) return;
        try { mSocket.close(); } catch (IOException ignored) { }
        mSocket = null;
    }
}
