.. SPDX-License-Identifier: GPL-2.0

Experimental background readahead candidate limit
================================================

Scope and defaults
------------------

CONFIG_READAHEAD_LIMIT defaults to n. When built, runtime enable defaults to
0. This experiment targets the dagu ARM64 4.19 tree and does not establish a
performance benefit. Its fixed threshold is si_mem_available() <= 1536 MiB
in base pages; its fixed extra candidate budget is eight base pages per
submission decision. It does not change F2FS sequential multipliers, ra_pages,
prev_pos, mmap_miss, compression selection, demand IO or filesystem filtering.

Control ABI
-----------

The root-only controls are under /sys/kernel/mm/readahead_limit:

* bind_fd (write only): decimal directory fd in the writing thread's fd table.
  Open the verified v1 /dev/cpuset/background directory in that same process
  (or inherit the fd). O_PATH directory fds are accepted. A number from another
  process's fd table is not usable. The controller helper acquires the exact
  online cpuset CSS reference. It rejects non-cgroup directories, ordinary
  files, wrong-controller hierarchies, v2, the root, descendants and paths
  other than /background. The trusted maintainer must first verify that this
  hierarchy is the system cpuset mount. A successful bind disables the policy;
  enabling always requires a separate write. The hot path matches this exact
  CSS object, not a name, descendant, UID or original IO submitter.
* enable (read/write): exactly decimal 0 or 1. Enabling without a live binding
  returns ENODEV. Disabling retains one bounded binding reference for reuse;
  replacement releases the old reference after all config readers finish.

Both writes require effective uid 0 and CAP_SYS_ADMIN in the initial user
namespace. Inputs consist of decimal digits with at most one final newline;
empty, signed, embedded NUL, whitespace, trailing junk and overflow inputs fail.
Failures preserve the old config, enabled state and generation. A new successful
write publishes an immutable config and increments its generation. No budget
or pressure threshold is writable. Repeated disable does not accumulate refs.
The built-in control group has no module unload path.

Binding pins lifetime, not online state. Each decision rechecks the target and
current CSS online/non-dying status under RCU. Removing/recreating a same-named
group never transfers the binding; bind again after verifying the new object.
Concurrent migration/offline can yield either instantaneous snapshot. Unknown
identity or pressure leaves the original candidate unchanged. A successful
disable waits for prior config readers, but an earlier decision can submit IO
after the write returns. It does not cancel IO, undo cache/markers, or restore
an earlier file readahead history. Only later decisions are disabled.

Window contract
---------------

All ranges are base-page half-open intervals. W0 is the raw candidate after
native growth/merge, before EOF/cache/allocation filtering. D is the remaining
buffered request envelope or exactly one current mmap fault page. A hint such
as mmap ra_pages is never D. Invalid/overflowing endpoints or async > size
leave the original tuple unchanged. Zero W0 or empty D remain unchanged.

Under the matched background and low-memory snapshot, retain W0 intersect D,
then add up to eight pages on the right and the remaining budget on the left.
When disjoint, retain W0's nearest eight pages toward D. F stays within W0 and
contains its full intersection with D. EOF never exempts a raw candidate from
this calculation: a concurrent file growth must not expand a previously
approved 128-page candidate beyond the budget.

If F equals W0, all original tuple fields and marker behavior are preserved.
Only a strict clip updates start/size/async in the ra_submit path and submits
the same local tuple. New async is min(original async, F's pages after D.end).
A zero async creates no marker. Direct small random candidates only change
local parameters; no new ra history is invented. The ordinary demand fallback
continues to fetch requested data not included in the original candidate.

Five private MM contexts cover buffered sync/async and mmap sequential,
readaround and async. Native hints/multipliers run first. Known-context force
windows are limited once after native max_pages and before 2 MiB chunking;
lookahead stays zero and file ra history is untouched. In particular an fd
with POSIX_FADV_RANDOM combined with a MADV_SEQUENTIAL VMA has D=one page even
when its hint/raw window is 128 pages. Public APIs with unknown callers,
readahead(2) and FADV_WILLNEED retain their original behavior. No second budget
is applied in __do_page_cache_readahead. CONFIG=n filemap call sites use the
original public APIs directly.

Decision diagnostics
--------------------

filemap:filemap_readahead_decision is disabled by default. Each record contains
source, D, raw and final tuple, force-path flag, the current/target identity,
available base pages actually used, and configuration generation. All fields
are copied before IO; no CSS/config pointer escapes RCU. A missing pressure
sample is -1. Identity zero means not evaluated/unknown. Reasons are:

0 DISABLED; 1 UNKNOWN_SOURCE; 2 UNKNOWN_IDENTITY; 3 NOT_BACKGROUND;
4 UNKNOWN_PRESSURE; 5 NOT_LOW; 6 UNKNOWN_RANGE; 7 WITHIN_BUDGET (also empty
ranges); 8 LIMITED; 9 UNSUPPORTED_EXPLICIT (public force with unknown D).
The force flag distinguishes the known-context force pipeline independently
of its limiting reason. Available memory is sampled only for matched live
background identities. Zero available pages is a valid LOW sample.

The older readahead_window event resamples later for observation; its identity
and available value are not the policy decision inputs. Candidate pages are
not actual storage IO pages. EOF, cache, allocation and filesystem submission
can remove candidates, and filesystem IO units can differ from base pages.

Validation and rollback
-----------------------

Host extraction tests run the actual numeric/parser helpers against literal
contract examples and input bounds; they are not kernel lifetime, RCU, fault,
IO error, shared-fd or performance tests. The bounded readahead/probe.c supports
mmap --fd-random --sequential to exercise the mixed fd/VMA force path. Its host
checks validate option handling and data, not that an Android kernel clipped.

Before accepting ticket02, boot a newly built experimental kernel and run the
existing bounded read/mmap fixtures with events in a separate instance filtered
by test PID/dev/inode, 256 KiB per CPU, for one bounded operation at a time.
Confirm true decision records for all five contexts, and the mixed fd/VMA
force case (D1/hint128, raw128 -> final9 if native max_pages permits128).
Use both threshold sides, bound/nonmatching/unknown identity and disabled
states; validate returned bytes, EOF/zero/large requests, recovery and markers.
Retain FIEMAP before/after and DATA-sector filtering for IO attribution; discard
lost/nested/extent-changing trials. Do not substitute subsequent window samples
for decision fields. Also exercise invalid control inputs, concurrent changes,
shared fds, migration, offline/recreation, truncation and allocation/IO errors.
No host test or ticket01 GO closes these real-kernel gates.

Rollback is enable=0 for subsequent decisions, or rebuild CONFIG_READAHEAD_LIMIT=n
and boot that kernel. Existing IO/history is never rolled back. Use fresh opens
for comparisons requiring untouched readahead history. Performance comparisons
keep diagnosis off except for separate symmetric confirmation trials.
