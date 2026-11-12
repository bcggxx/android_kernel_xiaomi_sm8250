/* SPDX-License-Identifier: Apache-2.0 */
package com.android.server.wm;

import android.os.IBinder;
import android.os.RemoteException;
import java.util.NoSuchElementException;

public final class DeathRegistrationTest {
    // Fixed AOSP15 BinderProxy JNI: missing recipient throws; DEAD_OBJECT returns true.
    private static final class RemoteBinder implements IBinder {
        DeathRecipient registered;
        boolean failLink;
        boolean dead;
        boolean falseResult;
        int attempts;
        RuntimeException unexpected;
        public void linkToDeath(DeathRecipient recipient, int flags) throws RemoteException {
            if (failLink) throw new RemoteException();
            registered = recipient;
        }
        public boolean unlinkToDeath(DeathRecipient recipient, int flags) {
            attempts++;
            if (unexpected != null) throw unexpected;
            if (registered != recipient) throw new NoSuchElementException();
            // DEAD_OBJECT is mapped to JNI_TRUE just like NO_ERROR.
            if (!dead) registered = null;
            return !falseResult;
        }
    }
    public static void main(String[] args) throws Exception {
        RemoteBinder binder = new RemoteBinder();
        MiMemEngineDeathRegistration registration = new MiMemEngineDeathRegistration(binder);
        registration.link(() -> {});
        // WM done wins the CAS after link: prepare cleans up, then queued END cleans up.
        registration.unlink();
        registration.unlink();
        if (binder.registered != null || binder.attempts != 1) throw new AssertionError();
        System.out.println("PASS cancellation cleanup twice consumes one registration");
        binder = new RemoteBinder();
        binder.failLink = true;
        registration = new MiMemEngineDeathRegistration(binder);
        try { registration.link(() -> {}); throw new AssertionError(); }
        catch (RemoteException expected) { }
        registration.unlink();
        if (binder.attempts != 0) throw new AssertionError();
        System.out.println("PASS failed link leaves nothing to unregister");
        binder = new RemoteBinder();
        registration = new MiMemEngineDeathRegistration(binder);
        registration.link(() -> {});
        binder.registered = null; // Recipient no longer in BinderProxy JNI list.
        registration.unlink();
        registration.unlink();
        if (binder.attempts != 1) throw new AssertionError();
        System.out.println("PASS recipient already absent is bounded cleanup");
        binder = new RemoteBinder();
        registration = new MiMemEngineDeathRegistration(binder);
        registration.link(() -> {});
        binder.dead = true;
        registration.unlink();
        registration.unlink();
        if (binder.attempts != 1) throw new AssertionError();
        System.out.println("PASS DEAD_OBJECT success is consumed once");
        binder = new RemoteBinder();
        registration = new MiMemEngineDeathRegistration(binder);
        registration.link(() -> {});
        binder.falseResult = true;
        registration.unlink();
        registration.unlink();
        if (binder.attempts != 1) throw new AssertionError();
        System.out.println("PASS false unlink result is not retried");
        binder = new RemoteBinder();
        registration = new MiMemEngineDeathRegistration(binder);
        registration.link(() -> {});
        RuntimeException failure = new IllegalStateException("unexpected Binder failure");
        binder.unexpected = failure;
        try { registration.unlink(); throw new AssertionError(); }
        catch (IllegalStateException expected) {
            if (expected != failure) throw new AssertionError();
        }
        registration.unlink();
        if (binder.attempts != 1) throw new AssertionError();
        System.out.println("PASS unrelated runtime failure propagates without retry");
        binder = new RemoteBinder();
        registration = new MiMemEngineDeathRegistration(binder);
        registration.link(() -> {});
        registration.unlink();
        if (binder.registered != null) throw new AssertionError();
        System.out.println("PASS next launch registration still usable");
    }
}
