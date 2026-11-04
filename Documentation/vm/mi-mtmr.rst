MTMR ordinary base-file nomap worker
===================================

CONFIG_MI_MTMR (default n) enables a separate, runtime-disabled control:
``/sys/kernel/mi_mtmr/nomap_enable``. Root may explicitly write 1 to start
one page worker, or 0 to close admission and wait for all pending/running
pages and in-flight producers. The slab worker and C05 contention worker
have separate controls. No prefetch policy or unused prefetch API is added.

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
