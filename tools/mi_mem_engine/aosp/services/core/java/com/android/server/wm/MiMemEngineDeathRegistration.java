/* SPDX-License-Identifier: Apache-2.0 */
package com.android.server.wm;

import android.os.IBinder;
import android.os.RemoteException;

import java.util.NoSuchElementException;

/** Worker-owned death registration. Binder callbacks only enqueue cancellation. */
final class MiMemEngineDeathRegistration {
    private final IBinder mBinder;
    private IBinder.DeathRecipient mRecipient;

    MiMemEngineDeathRegistration(IBinder binder) {
        mBinder = binder;
    }

    void link(IBinder.DeathRecipient recipient) throws RemoteException {
        mBinder.linkToDeath(recipient, 0);
        mRecipient = recipient;
    }

    void unlink() {
        IBinder.DeathRecipient recipient = mRecipient;
        mRecipient = null;
        if (recipient == null) return;
        try {
            // Consume before Binder I/O. DEAD_OBJECT is already success in AOSP15 JNI.
            mBinder.unlinkToDeath(recipient, 0);
        } catch (NoSuchElementException alreadyRemoved) {
            // Binder death may already have removed the remote recipient.
        }
    }
}
