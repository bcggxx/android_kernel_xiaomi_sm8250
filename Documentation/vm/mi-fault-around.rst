UID fault-around policy (experimental)
=====================================

CONFIG_MI_FAULT_AROUND is a bool, default n. Its independent runtime switch
also defaults to false. It does not change the readahead-limit window policy
or global fault_around_bytes. Only do_read_fault's existing do_fault_around
call is gated. Existing map_pages, UFFD, VMA/PTE range, demand fault, retry
and error handling remain in place. Disabling affects subsequent decisions;
it does not undo existing PTEs, page cache or submitted IO.

Controls (built-in object mi_fault_around)
-----------------------------------------

Both parameters are root-only mode 0600::

 /sys/module/mi_fault_around/parameters/enabled
 /sys/module/mi_fault_around/parameters/fault_around

The first accepts the kernel's standard boolean syntax. The second accepts
one decimal u32 per write (kstrtouint base 10). Zero clears all 512 bits;
1..9999 returns EINVAL; a nonzero value with value % 10000 > 511 returns
ENOSPC; otherwise that remainder's bit is set, accumulating previous writes.
Parse/range errors preserve the old bitmap. Repeated writes are idempotent.
There is no single-bit deletion. Readback is an ascending space-separated
list of 10000 + bit index, followed by newline (empty list: newline only).
It does not preserve the originally supplied UID's high digits.

Writers and readback serialize under a mutex. Each bitmap bit is accessed
with atomic bit operations; the fault path does not take that mutex. Clearing
is deliberately bit by bit, so fault readers can see intermediate states.
There is no atomic 512-bit replacement guarantee. A reader that already
observed enabled may finish its decision while disable is being written.

Suggested maintenance sequence: disable, write 0, add desired entries, read
back, then enable. This is a supported administrator policy interface;
no automatic ROM daemon or production policy is assumed.

Decision
--------

When disabled the policy allows the original fault-around path. When enabled,
u = __kuid_val(current_uid()) uses the subjective credential's uid, including
an override_creds context; it is neither euid, fsuid nor a namespace-mapped
integer. u < 10000 skips fault-around. Otherwise i = u % 10000: i > 511
allows it, and other values allow it only if bit i is clear. For example,
10000 and 20000 collide on bit 0, 10511 selects bit 511, and 10512 is always
allowed. INVALID_UID follows the same u32 arithmetic. This does not provide
Android multiuser isolation or standard Android appid normalization.

Source and adaptation boundary
------------------------------

This is an independent GPL-2.0 4.19 implementation, not recovered donor C.
The madrid dynamic_readahead.ko SHA256 is
632f666f72c540e5239873fbb3adb93d9623055fc228662c8d5638d7c1566359.
Its callback/setter/getter disassembly establishes the modulo/bitmap behavior.
The screenshot subset 96dd2f7849a880cb693f371bc0fc8595645dda2f has no matching
C source; its root license is not used to infer missing implementation rights.

Official android common commit 01c7e6b174af760d2ae7d975ab8d6f2597c2d1ec,
include/trace/hooks/mm.h and mm/memory.c, fixes the should_fault_around hook
polarity: false skips the speculative path while demand fault remains.
Official cred.h/cred.c, cross-checked against bundled boot get_task_cred and
commit_creds disassembly, supports current subjective cred uid interpretation
of the donor callback's task+2312/cred+8 loads. The target uses credential APIs,
never those offsets. Original artifacts and fixed-source hashes are archived
in the workspace sm8250/artifacts/dagu/c13-cred-audit and launch-analysis.
Boot banner and ko vermagic differ: this evidence does not prove loading ABI
compatibility, identical vendor types, or runtime performance. The donor fault
callback had no enable gate; independent default-off control and root-only
administrator configuration are explicit target safety adaptations.

Validation limits
-----------------

The dagu host check extracts control/decision functions, covering UID limits,
bitmap collisions, errors, readback and disable. It does not exercise real
mmap faults, current credentials, kernel bitop concurrency or UFFD. Object
compilation is not boot testing. Real file mmap/map_pages versus demand page
observation remains unaccepted; no functional, regression or performance
PASS is claimed. No device setting is enabled by this patch.
