C22 prepared refill reserve
==========================

Independent dagu 4.19 adaptation of the C22 refill contract, following the
existing local C21 prepared-ref1 lifecycle; no donor instruction copying.
CONFIG_MI_RECLAIM_POOL defaults to n and runtime enabled defaults to false.
It does not change the old staging MI_RECLAIM reclaimer.

Control is /sys/kernel/mi_reclaim_pool/enabled (boolean), stats is read-only.
ON acquires the same allocator_reserve owner as C21 and returns -EBUSY on
conflict. OFF excludes new transfers under pool_lock, unregisters the shrinker,
stops refill, drains owned pages, then releases owner. Static builtin metadata
has no unload lifetime; already borrowed pages belong to their callers.

The reserve holds at most 1024 ordinary prepared ref1 order-zero NORMAL pages
(4 MiB for dagu's 4 KiB pages), requires this to be <= totalram_pages/512, and
refills below 512 pages. Allocation uses no direct reclaim, high watermark plus
remaining budget, with at least 300 ms failure/pressure backoff. A generation
check rejects in-flight refill after pressure/stop. Shrinker counts and scans
are base pages by nid; no progress returns SHRINK_STOP, empty count SHRINK_EMPTY.

After ordinary fast/retry allocation failure and before direct reclaim, an
eligible current effective priority 0 <= prio < MAX_RT_PRIO may borrow from its
preferred NORMAL zone, subject to current ac high_zoneidx/nodemask/cpuset and
GFP/context constraints. Effective priority is sampled once, includes PI boosts
and excludes negative deadline priorities. Worker, atomic, PF_MEMALLOC, DMA,
DMA32, HIGHMEM, MOVABLE, MEMALLOC and NOFAIL are excluded. Borrowed pages do not
repeat prep_new_page; caller zero initialization is applied before the usual
outer memcg charge and allocation trace. A failed memcg charge frees normally.

stats refill_added = hits + pressure_returned + stop_drain + pages under the
pool lock. Counters are cumulative across sessions; misses means an eligible
borrow with no matching page. Free completion events only notify refill, they
do not capture any freed page. C22's true single/batch free capture, additional
reclaim policies and application QoS producer/revoke chain remain separate work.

Host selftest covers only actual extracted owner and borrow entry APIs. Required
real acceptance: force eligible order-zero slowpath with stocked/empty pool;
check selection, zero, memcg failure rollback and trace; pressure returns pages
and suppresses refill for >=300ms until high watermark recovery; race disable
against borrow/refill/shrinker, verify counters/conservation and owner conflict.
No boot, allocator functional, concurrency or performance claim follows from
object compilation or host extraction.
