C22 prepared refill reserve
==========================

Independent dagu 4.19 adaptation of the C22 refill contract, following the
existing local C21 prepared-ref1 lifecycle; no donor instruction copying.
CONFIG_MI_RECLAIM_POOL defaults to n and runtime enabled defaults to false.
It does not change the old staging MI_RECLAIM reclaimer.

Control is /sys/kernel/mi_reclaim_pool/enabled (boolean), stats is read-only.
The independent raw_enabled control defaults to false and can only be enabled
while the overall pool is enabled.
ON acquires the same allocator_reserve owner as C21 and returns -EBUSY on
conflict. OFF excludes new transfers under pool_lock, unregisters the shrinker,
stops refill, drains owned pages, then releases owner. Static builtin metadata
has no unload lifetime; already borrowed pages belong to their callers.

The reserve holds at most 1024 order-zero base pages (4 MiB for dagu's 4 KiB
pages), split between prepared ref1 pages and raw captured free pages. Raw is
limited to 512 pages and refill subtracts raw occupancy from the same total.
Enabling requires this total to be <= totalram_pages/512, and refill starts
below 512 prepared pages. Allocation uses no direct reclaim, high watermark
plus remaining budget, with at least 300 ms failure/pressure backoff. A
generation check rejects in-flight refill after pressure/stop. Shrinker counts
and scans are base pages by nid; no progress returns SHRINK_STOP, empty count
SHRINK_EMPTY.

After ordinary fast/retry allocation failure and before direct reclaim, an
eligible current effective priority 0 <= prio < MAX_RT_PRIO may borrow from its
preferred NORMAL zone, subject to current ac high_zoneidx/nodemask/cpuset and
GFP/context constraints. Effective priority is sampled once, includes PI boosts
and excludes negative deadline priorities. Worker, atomic, PF_MEMALLOC, DMA,
DMA32, HIGHMEM, MOVABLE, MEMALLOC and NOFAIL are excluded. Borrowed pages do not
repeat prep_new_page; caller zero initialization is applied before the usual
outer memcg charge and allocation trace. A failed memcg charge frees normally.

Raw capture is a separate opt-in path. A normal order-zero free is captured only
after free_pages_prepare() and the corresponding free-state check succeed, but
before the original PCP/buddy commit. The page remains refcount zero and is not
placed on the prepared chain. Batch capture first removes the caller's list
node. PGFREE is counted exactly once at capture. Raw borrow is preferred over
prepared borrow; allocation checks the page and calls prep_new_page() exactly
once before the usual outer memcg charge and trace.

Pressure and raw control disable drain raw pages through an explicit
free_one_page() return path using the current pageblock migratetype. They never
call __free_pages() or repeat free_pages_prepare on a raw page. Prepared pages
continue to use the ordinary ref1 __free_pages() path. raw_captured,
raw_rejected, raw_borrowed, raw_pressure_returned and raw_stop_drain are
cumulative statistics. C22's display/QoS selection and additional reclaim
policies remain separate work.

Host selftest covers only actual extracted owner and borrow entry APIs. Required
real acceptance: force eligible order-zero slowpath with stocked/empty pool;
check selection, zero, memcg failure rollback and trace; pressure returns pages
and suppresses refill for >=300ms until high watermark recovery; race disable
against borrow/refill/shrinker, verify counters/conservation and owner conflict.
No boot, allocator functional, concurrency or performance claim follows from
object compilation or host extraction.
