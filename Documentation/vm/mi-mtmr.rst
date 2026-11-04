MTMR ordinary base-file page worker
==================================

CONFIG_MI_MTMR (default n) enables a separate, runtime-disabled control:
``/sys/kernel/mi_mtmr/nomap_enable``. Root may explicitly write 1 to start
one page worker, or 0 to close admission and wait for all pending/running
pages and in-flight producers. The slab worker and C05 contention worker
have separate controls. ``readahead_enable`` independently enables the
readahead age policy on the same page worker; both runtime defaults are zero.

After a final base file unmap releases the memcg lock, the atomic producer
reserves metadata and tries LRU isolation. It never allocates, sleeps,
reclaims or waits under the PTE lock. There are exactly 1024 static metadata
slots, including producer reservations and running pages. At 4 KiB the
maximum isolated candidate capacity is 4 MiB. A successful isolation
transfers its single reference and increments NR_ISOLATED_FILE on its node.
C05 only accepts already isolated pages; these owners cannot both isolate
the same page. No donor page bits or task offsets are used.

Every accepted page wakes the worker immediately. After at most 32 pages
it yields and checks stopping/freezing again. When free pages are at or
below max(totalreserve_pages, 64 MiB in pages), admission fails and the
worker returns candidates to normal LRU instead of keeping a private
reserve. Every page also checks disabling and pressure before consumption.
This conservative threshold is a dagu safety policy, not a donor parameter
or a demonstrated performance optimum.

Remapping only marks cancellation. The worker uses a nonblocking page lock
check and returns mapped, dirty, writeback, mlocked, unevictable or detached
pages. Eligible pages use the normal force=false shrink_page_list path,
with ordinary reference and protection checks, no recursive C05 handoff,
and may_unmap=false checked directly before try_to_unmap. A remap may
briefly coexist with a queued candidate; this is not a synchronous LRU
return guarantee. Reclaim/putback consumes the sole isolation reference
and ends node isolation accounting exactly once.

The read-only ``state`` reports accepted, pending, running, reclaimed,
returned, producer inflight, capacity and batch. At a quiescent disabled
boundary, pending/running/inflight must be zero and accepted must equal
reclaimed + returned. While enabled, accepted = pending + running +
reclaimed + returned; producer reservations are separate and only become
accepted after successful isolation. A disable waits for reservations
before permitting another enable generation. Reboot also permanently
closes admission and drains the worker. A running normal reclaim call can
still delay disable; it is not forcibly interrupted.

The host extracted-function test covers bounded admission, owner
exclusion, accounting, cancellation marking, and finite disable drainage.
It does not establish PTE/LRU/RCU concurrency, real reclaim, suspend,
truncate or memcg-offline correctness. A real final file unmap -> queue ->
reclaim/remap putback -> disable target-memory test remains required.
No device enable, flash or reboot is implied. Compound and donor private
classification behavior remain C11/24b work; C11 is not complete.

Readahead age candidates
------------------------

The explicit phase1 ``ra_context`` demand range identifies speculative new
pages outside the entire current request. The internal ctx submission preserves
all original dynamic window/range/marker parameters. Legacy callers without
exact context, explicit force readahead, demand pages and cache hits are not
accepted. This restricted real producer coverage does not complete every
legacy readahead caller or the parent C11 capability.

After allocation, a bounded metadata reservation pins the page and records a
producer token. ``add_to_page_cache_lru`` records only successful insertion;
this common boundary covers ext4, f2fs, mpage, read_cache_pages and generic
readpage fallback. It does not claim IO completed. The original producer
publishes candidates only after all its read_pages calls return, or drops pins
for failed insertions/disabled/cancelled/pressure-invalid pages. Tokens cannot
outlive that call. A blocked original FS producer delays its inflight completion
and disable; there is no new wait/lock dependency on the worker or on page IO.
Its pin cannot safely be detached as a pending candidate before publication.

``readahead_age_ms`` defaults to 300, accepts 100..60000, and is writable only
while readahead is disabled and idle; bad/busy writes preserve the old value.
The shared pool has 1024 base-page slots total, including producers/running
owners, and the shared worker yields after at most 32 pages. Slots carry an
explicit kind and wrap-safe insertion timestamp. They never use a private page
flag or the standard page->lru while pending: RA pages stay on normal LRU and
hold exactly one extra metadata pin. This differs from the donor private age
buckets and can temporarily affect ordinary reclaim. Game, compound and private
LRU scheduling remain 25b; this implementation is not instruction equivalence.

At expiry the worker nonblockingly checks the page lock and rejects mapped,
referenced, dirty, writeback, errored, not-uptodate, detached or protected pages.
A successful isolate adds one isolation reference/NR_ISOLATED_FILE, then drops
the extra metadata pin before handing only the isolation reference to normal
force=false, may_unmap=false reclaim. Failed isolation or invalid candidates
only drop the metadata pin. Normal reclaim rechecks references/protection and
returns retained pages. IO still in flight never causes a blocking page lock.

Pressure releases pending pins independently of TTL: the worker polls at most
100ms while idle, and a global bounded shrinker directly drops up to 32 pending
metadata pins per scan without page/LRU locks or recursive reclaim. Zero scan
progress returns SHRINK_STOP, an empty count returns SHRINK_EMPTY; returned
objects are released pins, not a claim that physical pages were freed. Scans
also gate new RA admission/publication for 100ms. Active producers finish under
their original FS lifetime; already RUNNING normal reclaim cannot be forcibly
interrupted. Normal synchronous reclaim remains available throughout.

Nomap encountering an RA slot marks it cancelled and falls back for that event;
it never drops a pin in the PTE context or takes a second owner. PRODUCING and
RUNNING slots likewise exclude takeover. Once the existing owner finishes,
later eligible events can use the free slot. C05 can isolate an ordinary-LRU
RA page first; RA then fails isolation and releases its extra pin, so only the
actual successful isolator owns page->lru.

Policy disable clears only its enabled bit, waits its own producer reservations,
drains only its own pending slots and waits its own running owners. Disabling
nomap does not cancel RA; only the last disabled policy joins the shared worker.
Caller drainage can release pending pins even if the worker is frozen. Reboot
closes both policies before draining. Per-policy accepted = pending + running +
reclaimed + returned, and metadata get = put + live RA pins (including producers and running owners
before their pin release). At a disabled idle
RA boundary get=put and RA pending/running/inflight are zero. Statistics count
normal RA exits as returned, including pin-only exits with no isolation.

``test_mi_mtmr_ra.py`` extracts actual pin/producer/age/close/shrinker functions
with finite host adapters, including pressure cap/zero-progress and jiffies wrap.
It does not verify real FS, PTE, LRU, RCU or concurrent freezer behavior. Real
FS prefetch -> expiry -> normal reclaim/return -> disable still requires target
memory validation: functional behavior unconfirmed, regression unobserved and
performance unknown. No target enable, flash or reboot was performed.
