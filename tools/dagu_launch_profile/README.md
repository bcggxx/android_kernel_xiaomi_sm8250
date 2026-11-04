# Dagu launch profile: trusted collection, publication and replay

Independent default-off implementation for SM8250/dagu, Linux 4.19 and fixed
AOSP `android-15.0.0_r1`. `CONFIG_DAGU_LAUNCH_PROFILE` and
`sys.dagu_launch_profile.enabled` both default off. The kernel control ABI
remains DLP1; system_server and the root helper use bounded DLP2 packets. This
does not claim compatibility with the unknown Xiaomi iorap2 daemon.

## Collection and metadata

ActivityMetricsLogger creates a monotonic cookie at
`notifyActivityLaunching`. ActivityTaskSupervisor passes the exact
ActivityRecord/WindowProcessController pair after verified attachment. The
worker rechecks that identity under the WM lock, obtains a duplicate Binder
driver fd and strongly held ApplicationThread handle, and sends PREPARE,
BEGIN, FINISH or ABORT with the connection epoch and cookie.

Package metadata is queried outside the WM lock with
`getPackageInfoAsUser(package, GET_SIGNING_CERTIFICATES, userId)`. The wire
contains the package, user/UID, long version code, all current APK signer
SHA-256 digests in sorted order, and deduplicated base/split APK, native,
shared-file and resource-root paths. The metadata generation is a deterministic
SHA-256-derived scalar over that canonical set. FINISH queries and transmits
the full metadata again; a changed generation aborts rather than freezing the
launch.

Package update/removal/data-clear and user-removal broadcasts queue
INVALIDATE operations. PREPARE/BEGIN/FINISH/ABORT/INVALIDATE/OFF use a fixed
little-endian header and fixed ACK. A decoder rejects unknown flags, reserved
bytes, unsorted/duplicate paths, invalid lengths, generation mismatch, extra
descriptors and packets above 65535 bytes.

FINISH freezes the kernel snapshot and immediately opens its sole immutable
snapshot fd before the ACCEPT ACK. Digest and disk work runs on one helper
worker, so the Java control worker does not hold a Binder reference through
file hashing. The snapshot retains the existing collector budget until the
worker closes it.

At the resolved-PENDING association event, Java queues a metadata PREPARE
without waiting for attach. The helper validates the current package/user/UID/
version/signer/path generation against the stored 08a profile and enqueues
replay on a separate worker. The later collector BEGIN does not repeat PREPARE
unless metadata changed. PREPARE therefore covers the real pre-attach window;
the attach-after BEGIN is only collection start, never its PENDING substitute.

## Trusted file and profile publication

For every EXPORT_FILE fd, the helper checks read-only mode, resolves
`/proc/self/fd`, and requires an exact trusted APK/shared path or a descendant
of an advertised native/resource root. It then reopens each component with
`openat(O_NOFOLLOW)` and compares regular-file identity, size, mode, owner,
ctime and mtime. App-writable files, deleted/memfd/proc/sys/dev paths,
unadvertised paths, writable descriptions and identity races are skipped and
flagged.

APKs and native-library files receive a SHA-256 content digest. Shared
system/vendor/resource files retain mount/file identity but are not bulk-hashed.
The profile records a mount ID resolved from `/proc/self/mountinfo`, so inode
reuse alone is not a valid identity. Page-aligned snapshot ranges are clipped
to the real file EOF before storage; virtual rounded tail bytes are never
persisted.

Profiles live under `/data/misc/dagu_launch_profile/<user>/<package>/`.
`profile.bin` uses a CRC-checked little-endian schema with package metadata,
per-file identity/digest and byte ranges. Publication writes a unique tmp
file with `O_NOFOLLOW`, fsyncs it, atomically renames it and fsyncs the package
directory. Failure preserves the previous valid profile or leaves no profile.
Limits are 1 MiB per profile, 16 MiB and 64 packages in total, with oldest
profile eviction. Startup removes stale tmp files, rejects profiles from a
different boot/release/version, and verifies schema/CRC/ranges.

## Bounded replay

Replay reloads the stored profile and rechecks boot/release/version, metadata
generation, package, signers, path allow-list, mount ID, file dev/ino/size/
ctime/mtime and APK/native digest before reading. It uses a 128KiB scratch
buffer and synchronous `pread64`, not fadvise or a fake request count. Attempts
and successful bytes are each capped at 16MiB; at most 128 files and 1024
ranges are submitted. The 500ms window only prevents submitting the next chunk;
a synchronous pread already in flight may finish later.

A low-free-memory heuristic, cancellation epoch, new launch, done/abort,
package/user invalidation, disconnect and task death stop submission of further
chunks. OFF cancels replay, waits for the in-flight chunk to finish, waits for
profile publication, disables the kernel controller and ACKs before helper
process exit. An idle helper also observes the disabled property and exits
without claiming an OFF ACK. Short reads/errors, budget/window/pressure stops,
invalid profiles and actual completed bytes are logged separately.

Replay still cannot prove physical cold-cache I/O or bound pagecache occupancy;
synchronous pread is not hard-cancellable in a blocked filesystem call. Real
`PENDING_ATTACH`, Android init/SELinux, AOSP apply/build, Java/JNI/Soong,
filesystem crash durability, target concurrency and performance remain
unverified. Parent 08/C03 stays open, including pre-attach zygote/loader,
THP/filemap, writable donor data and donor-difference coverage.

## Host check

```
cc -std=gnu11 -Wall -Wextra -Werror -pthread \
  -Itools/dagu_launch_profile -Itools/dagu_launch_profile/include \
  tools/dagu_launch_profile/tests/profile_publish_test.c \
  tools/dagu_launch_profile/dlp_profile_store.c \
  tools/dagu_launch_profile/dlp_sha256.c -o /tmp/dlp-profile-publish-test
/tmp/dlp-profile-publish-test
```

The check drives the production DLP2 parser, snapshot-to-profile publisher and
profile replay. It covers metadata-generation mismatch, page-range EOF
trimming, real APK digest, profile readback, atomic non-replacement, bounded
actual pread, immediate cancellation and package invalidation. It does not
prove Android Java/JNI/Soong/SELinux, HyperOS integration, kernel RCU/PTE/FS
concurrency, device launch or physical IO.
