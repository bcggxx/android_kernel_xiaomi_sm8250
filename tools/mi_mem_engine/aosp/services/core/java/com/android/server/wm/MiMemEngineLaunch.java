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

/** Fixed AOSP15 warm/hot launch adapter. Entry points require the WM global lock. */
final class MiMemEngineLaunch {
    private static native int[] binderIdentity(IBinder binder);
    private static final AtomicLong NEXT_COOKIE = new AtomicLong();
    private final ActivityTaskSupervisor mSupervisor;
    private final Handler mWorker;
    private LocalSocket mSocket; // Worker only; disconnect revokes controller.

    static final class Session {
        final long cookie = NEXT_COOKIE.incrementAndGet();
        final WindowProcessController process;
        final int pid;
        final int uid;
        final IBinder thread;
        // 0 pending; 1 BEGIN committed/in flight; 2 cancelled. CAS orders done vs BEGIN.
        final AtomicInteger state = new AtomicInteger();
        int handle;
        IBinder.DeathRecipient death;
        Session(WindowProcessController process) {
            this.process = process;
            pid = process.getPid();
            uid = process.mUid;
            thread = process.getThread().asBinder();
        }
    }

    MiMemEngineLaunch(ActivityTaskSupervisor supervisor) {
        mSupervisor = supervisor;
        HandlerThread thread = new HandlerThread("MiMemEngineLaunch");
        thread.start();
        mWorker = new Handler(thread.getLooper());
    }

    Session begin(WindowProcessController process) {
        if (process == null || process.getPid() <= 0 || process.getThread() == null) return null;
        Session session = new Session(process);
        mWorker.post(() -> prepare(session));
        return session;
    }

    void end(Session session) {
        if (session == null || session.state.getAndSet(2) == 2) return;
        mWorker.post(() -> {
            try {
                if (mSocket != null) {
                    send(2, session, null);
                    Slog.d("MiMemEngineLaunch", "END cookie=" + session.cookie
                            + " tid=" + session.pid);
                }
            } catch (IOException e) {
                disconnect();
            }
            if (session.death != null) session.thread.unlinkToDeath(session.death, 0);
        });
    }

    private void prepare(Session session) {
        FileDescriptor binderFd = null;
        try {
            if (session.state.get() == 2) return;
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
                        || session.state.get() == 2) return;
            }
            session.death = () -> end(session);
            session.thread.linkToDeath(session.death, 0);
            if (!session.state.compareAndSet(0, 1)) {
                session.thread.unlinkToDeath(session.death, 0);
                return;
            }
            if (mSocket == null) {
                mSocket = new LocalSocket(LocalSocket.SOCKET_SEQPACKET);
                mSocket.connect(new LocalSocketAddress("mi_mem_engine",
                        LocalSocketAddress.Namespace.RESERVED));
                mSocket.setSoTimeout(200);
                if (mSocket.getPeerCredentials().getUid() != 0) {
                    throw new IOException("untrusted mi_mem_engine peer");
                }
            }
            send(1, session, binderFd);
            Slog.d("MiMemEngineLaunch", "BEGIN cookie=" + session.cookie
                    + " tid=" + session.pid);
            // done racing a committed BEGIN is ordered behind it on this worker.
            if (session.state.get() == 2) send(2, session, null);
        } catch (IOException | RemoteException e) {
            disconnect();
            end(session);
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
            throw new IOException("mi_mem_engine request rejected");
        }
    }

    private void disconnect() {
        if (mSocket == null) return;
        try { mSocket.close(); } catch (IOException ignored) { }
        mSocket = null;
    }
}
