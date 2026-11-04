# Dagu launch profile: ticket07 collection slice

Independent clean implementation for SM8250/dagu, Linux4.19 and fixed
AOSP `android-15.0.0_r1`. Both `CONFIG_DAGU_LAUNCH_PROFILE` and the runtime
`sys.dagu_launch_profile.enabled` property default off. This is the new DLP1
protocol; it does not implement the unknown Xiaomi iorap2 daemon ABI.

The combined `../mi_mem_engine/aosp/frameworks-base.patch` and apply script
install both independent adapters. C03 does not consult C06's enable property.
Build `dagu_launch_profile_helper`, add its init rc, merge this directory's
policy into platform-private policy and label `/dev/dagu_launch_profile`.
`include/linux/dagu_launch_profile.h` is the exported kernel UAPI copy;
keep it identical when changing the ABI. Actual platform JNI/Soong/SELinux
integration and HyperOS adaptation still require a platform build.

ActivityMetricsLogger allocates a monotonic cookie with LaunchingState at
notifyActivityLaunching. Resolved package/user and exact ActivityRecord token
are bound at notifyActivityLaunched. Without a thread this remains pending.
ActivityTaskSupervisor.realStartActivityLocked passes the same r/proc pair
immediately after r.setProcess(proc), including verified cold attach from
AMS; no delayed numeric PID or package lookup finds the owner. If attach has
already happened, association captures r.app directly. The worker rechecks
the exact WPC/thread under the WM lock, then exports a duplicate of the existing
Binder driver file plus a strongly held ApplicationThread handle. The kernel
resolves the actual Binder owner and compares tid/uid. The initial attach
claim depends on the fixed AOSP AMS startSeq/UID validation; it is not a claim
about an unmodified proprietary ROM.

Session CAS prevents done/abort-before-attach from resurrecting a cookie.
Death registration is consumed exactly once using the existing fixed C06
helper. One worker orders BEGIN/FINISH/ABORT. A socket failure revokes the
controller and advances the Java connection generation, preventing queued
old-generation pending launches from restarting on reconnect. Each helper
connection uses a new boottime epoch. New launches abort the previous active
collector. Coalescing to a different ActivityRecord conservatively aborts
that C03 cookie rather than reassigning its identity. A 1000ms collection
lease is below the kernel's 2000ms hard limit. A timed-out collector yields
an error and reconnect, never publishes an incomplete successful profile.

On successful metrics done the actual helper FINISHes, receives the immutable
snapshot fd, exports each original read-only file fd, fstats them, and decodes
every LE range with size/index/order/overflow/page-alignment and 32MiB bounds.
Page-rounded tail bytes beyond EOF are valid when the start is within EOF.
Files whose original description is writable are not exported; their metadata
ranges are consumed and explicitly counted as skipped. All fds close on every
success/error path. The summary records flags, dropped and skipped counts;
truncation is visible. No disk training profile is published by ticket07,
including truncated snapshots. Ticket08 supplies package identity validation,
atomic persistence, invalidation and bounded actual pread replay.

Coverage explicitly starts after verified ApplicationThread attachment.
Pre-attach zygote/loader accesses are not collected. Coalesced launches and
writable original file descriptions have the additional exclusions above.
This is not full-process tracing or proof of donor equivalence.

A single host production-function seam check is available (Linux container):

```
cc -std=gnu11 -Wall -Wextra -Werror -Itools/dagu_launch_profile/include \
  tools/dagu_launch_profile/tests/snapshot_consumer.c -o /tmp/dlp-snapshot-consumer
/tmp/dlp-snapshot-consumer
```

The harness compiles the real helper and calls its actual snapshot consumer;
it accepts a page-rounded two-page short-read result for a 5000-byte file and
rejects an aligned u64 length that would wrap budget subtraction. It does not
prove Android Java compilation, real launch/attach, Binder owner resolution,
PTE/RCU concurrency, device IO or performance. After the green run, static
review added the rounded-EOF guard and expected-WPC check; final source was
not recompiled or rerun under the one-check budget. No device, CI, full kernel or
AOSP build was run for this slice.
