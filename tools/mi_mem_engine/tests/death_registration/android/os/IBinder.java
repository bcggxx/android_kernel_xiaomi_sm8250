/* SPDX-License-Identifier: Apache-2.0 */
package android.os;
/** Host boundary only; production class is compiled unchanged. */
public interface IBinder {
    interface DeathRecipient { void binderDied(); }
    void linkToDeath(DeathRecipient recipient, int flags) throws RemoteException;
    boolean unlinkToDeath(DeathRecipient recipient, int flags);
}
