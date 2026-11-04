# C06 warm/hot production plan

Fixed producer baseline: AOSP frameworks/base android-15.0.0_r1; target kernel SM8250/dagu 4.19.325. Runtime is disabled by default. No Android build/device validation is available in this worktree.

## Public boundaries and design change

Real ActivityMetricsLogger successful existing-process launches and done/abort drive a per-TransitionInfo cookie. A dedicated serial worker performs socket and JNI I/O outside the WM global lock. done atomically cancels pending work before queuing END. The worker CAS from pending to committed-BEGIN linearizes against cancellation: cancellation that wins suppresses BEGIN; an already committed in-flight BEGIN is followed by END and remains bounded by the 1000ms lease. This is the explicit in-flight allowance; no WM thread waits for an IPC operation. Coalesced launches replace the tracked process/session, and warm-to-cold restarts revoke it.

The contract originally named private Binder transport. This slice substitutes an init-created AF_UNIX SOCK_SEQPACKET socket. The root helper checks SO_PEERCRED UID 1000 and exact SELinux peer context system_server; policy restricts socket access to system_server. Producer checks server UID 0. SCM_RIGHTS transfers a duplicated system_server Binder driver descriptor. The helper alone holds the exclusive CAP_SYS_ADMIN kernel controller. Default-disabled init service and trusted sys.mi_mem_engine.enabled opt-in gate runtime use.

## Exact identity

Do not convert /proc stat ticks to invented exact birth nanoseconds. A pidfd-open followed by IApplicationThread.pingBinder would also be insufficient: fixed AOSP attachApplication validates the caller PID/UID/startSeq but does not prove the supplied IApplicationThread Binder belongs to that caller. A process can supply a different living Binder.

JNI exports a dup of ProcessState's real Binder driver FD plus the remote IApplicationThread handle. The Java Session holds the original Binder strongly until the worker has finished, preventing handle reuse, including cancellation in flight. Kernel binder_mme_get_task validates the driver-file origin UID1000 (transport separately authenticates system_server SELinux), resolves that exact strong-reference node under Binder locks, obtains its owning task and checks expected WPC main TID/UID. MME pins struct pid and exact task birth time; PID reuse and a forged remote-thread owner both fail closed. No new public Binder ioctl is added. Native local/RPC Binders are rejected.

## Verification and limits

Pre-agreed seams are real ActivityMetricsLogger→helper socket→kernel controller and allocator trace; no fake Java model tests. Locally check patch applies to fixed source bytes and compile helper with strict warnings against the copied, unchanged shared UAPI and system exported Linux types. Android platform build, JNI link/API build, SELinux policy compilation and warm-launch/drawn/abort/death/timeout E2E remain required downstream. Cold pending/attach and SurfaceFlinger input remain 13b/13c. This is source integration for a modifiable fixed AOSP ROM, not evidence that stock HyperOS invokes it.
