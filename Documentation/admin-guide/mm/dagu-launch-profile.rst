Dagu launch profile collection and publication
==============================================

This is an independent, default-off 4.19 implementation motivated by the
iorap2 binary collector/compiler/export evidence. It does not implement or
claim compatibility with the donor ioctl protocol or daemon. Enable
CONFIG_DAGU_LAUNCH_PROFILE only in an explicitly selected build; the 0600
/dev/dagu_launch_profile controller still starts disabled. All ioctls and
snapshot reads require CAP_SYS_ADMIN in the initial user namespace.

The fixed 64-byte request in linux/dagu_launch_profile.h has identical compat
layout (aligned u64 fields). ENABLE sets a connection epoch and turns runtime
collection on/off. BEGIN requires a monotonically increasing nonzero launch
cookie, epoch, lease <=2000ms, and a duplicated Binder driver fd plus strong
remote handle from system_server. The existing Binder bridge resolves the
node's real owner; expected pid/uid must match. A stable tgid reference filters
hooks; there is no deferred numeric pid lookup. BEGIN preallocates all slots.

FINISH removes collection eligibility, waits for old RCU readers, sorts and
merges adjacent/overlapping ranges, and freezes the result synchronously.
ABORT, controller close, disable, death observation or lease expiry discard
uncommitted data. Death is observed on a 50ms process-context work poll. An
expired session rejects hook events immediately; reclamation follows on work.
The control mutex never appears in hooks; no hook allocates, resolves paths,
copies to users or performs final fput. Teardown synchronizes RCU before
process-context reference drops. The backend is built-in, not unloadable.

Collection covers successful buffered-copy bytes and ordinary file PTEs after
set_pte_at in alloc_set_pte. Both fault-around and finish_fault use that point.
Existing PTE returns, COW and huge PMD installation are excluded. THP mapping
coverage and pre-attach zygote/loader accesses are explicit gaps. Offsets are
page-rounded from the actual copied prefix and actual VMA PTE address; zero
copies and errors contribute nothing. This is not a full process trace.

There is one active session, at most 128 pinned file identities and 4096
unique page records. Hooks index the file object only. At freeze, different
opens of the same pinned inode/mount/mapping merge;
no bare dev/ino identity survives a file reference. Unique-page budget is
32MiB, additionally bounded by the 4096 per-open records (16MiB with 4KiB
pages). Independent opens can therefore exhaust the raw budget before the
freeze-time union reaches 32MiB; this is conservative truncation, not silent
loss presented as complete. Mutable-file/version acceptance belongs to the
helper validation and persistence path; pinning an inode alone proves no
content immutability.
Preallocation plus maximum wire image is below 4MiB. Slot exhaustion preserves
a prefix, sets TRUNCATED and saturating dropped-event count. A truncated
snapshot must never be published as successful training. Spinlock work is
bounded by these caps and a hard two-page work limit per hook (at most
8192 slot comparisons). Current read/PTE callsites contribute one page. Larger
future callers truncate after the bounded prefix. Hook latency still needs
target measurement.

SNAPSHOT returns a readonly anonymous fd directly as the ioctl return value;
there is no post-install user copy. Bytes are little endian: 32-byte header
(magic 0x31504c44, version, byte size, file count, range count, flags, page
shift, dropped events), then 24-byte records (u32 index/reserved, u64 byte
offset/length). Reads use immutable bytes and bounded offsets. EXPORT_FILE
reopens a readonly independent file description by index only if the original open was
O_RDONLY with FMODE_READ. Writable descriptions are rejected with EACCES;
the helper skips them. Export checks read permission and revalidates pinned
inode/mount identity and regular/read-only mode. Independent descriptions avoid
sharing the app f_pos/f_flags. Export never upgrades file access. Snapshot close drops
its reference; controller close cannot leave dangling snapshot memory.
Disable revokes snapshot reads with ESHUTDOWN (already completed reads cannot
be recalled). A user-held snapshot blocks the next BEGIN and still charges
its bounded storage. At most one snapshot fd is issued at a time.

Status returns 1 for active, 0 otherwise. Invalid version/size/unused fields
return EINVAL; unsupported operation ENOTTY; disabled EACCES; identity/cookie
or epoch mismatch ESRCH; privilege EPERM; allocation ENOMEM; occupied budget
ENOSPC; user-copy failure EFAULT before mutation. Original file export fds
are ordinary readonly files: disable does not revoke reads already authorized
through those fds. The trusted helper closes them on cancellation.

The fixed AOSP15 system_server adapter queries package/user/UID/version/signer
and application path metadata outside the WM lock. It sends a bounded
little-endian DLP2 PREPARE/BEGIN/FINISH/ABORT/INVALIDATE/OFF stream to the
root helper. FINISH re-queries the same metadata and a changed generation
aborts instead of producing a profile. Package update, removal, data-clear and
user-removal broadcasts invalidate the corresponding stored profile.

The helper consumes only the immutable snapshot plus independently reopened
readonly file fds. It requires the resolved path to match trusted package
metadata, reopens every component with ``O_NOFOLLOW``, compares file identity
and ownership, hashes APK/native content, and records a mount ID. Page-rounded
snapshot tails are clipped to the real file size. Profiles are CRC-checked,
little endian, per-user/package, atomically renamed, and bounded to 1 MiB per
profile and 16 MiB/64 packages in total. A different boot, release or version,
corrupt schema, truncated snapshot or metadata-generation mismatch cannot
replace the previous valid profile.

This is default-off publication only. Real ``PENDING_ATTACH`` launch replay,
bounded pread, in-flight cancellation and the replay off acknowledgement remain
ticket 08b. Host parsing checks and object compilation cannot prove Android
integration, SELinux enforcement, PTE concurrency, RCU behavior, filesystem
atomicity, cold-cache I/O or performance. No image or runtime enablement is
implied by building these sources.

FINISH revalidates the lease and task after detaching collection and waiting
for readers. An expired or exited session is dropped with ESRCH even if the
50 ms expiry worker has not run. ABORT still drops an invalid session without
publishing a snapshot. A successful validity check is the commit boundary;
this does not promise that the task remains alive after that point.
