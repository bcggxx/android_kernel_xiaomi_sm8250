Order-zero/one allocator reserve (experimental)
==============================================

CONFIG_MI_MEMPOOL is built in, defaults to n, and requires 4 KiB ARM64 pages
and no memory hotplug. Runtime enabled defaults to 0. It is an independent
4.19 adaptation of the observed mi_mempool failure channel, using the GPL-2.0
Linux allocator, shrinker, kthread and sysfs APIs in this tree. No donor C or
unknown vendor page fields are copied. This implements orders zero and one;
orders 4/5 and the pre-reclaim policy remain separate work.

Root can write 1 or 0 to /sys/kernel/mi_mempool/enabled. Enabling refuses a
reserve-owner conflict or capacity above total RAM / 512. Order zero has high/low 1024/512 blocks (4 MiB maximum); order one has
high/low 128/64 contiguous two-page blocks (1 MiB maximum). The combined
limit is 5 MiB. One sleeping worker fills
the local NORMAL zone. An empty reserve is a valid enabled state.
/sys/kernel/mi_mempool/stats reports current blocks/pages, high/low,
cumulative hits/misses, refill failures, pressure returns and stop drains.
Hits are cumulative loans, not outstanding references.

The only allocation consumer is an unsuccessful __alloc_pages_slowpath,
before the original memcg charge and allocation trace. It must be a
sleepable order-zero or order-one request with DIRECT_RECLAIM, without DMA, DMA32,
HIGHMEM, MOVABLE, ATOMIC, MEMALLOC or NOFAIL. Interrupt/atomic contexts,
PF_MEMALLOC and the refill worker are rejected. The preferred allocation
zone must be NORMAL and satisfy the allocation context's high_zoneidx,
nodemask and current cpuset. Only a reserve page from that exact zone can
be returned. Other requests retain the original failure result. ZERO and
init-on-alloc initialization run before the original memcg charge; charge
failure uses ordinary __free_pages with the original order. Order-one
ZERO/init clears both base pages; caller COMP constructs compound state
once after leaving the pool. Without COMP the block stays non-compound. Fast-path success is unchanged.

Stored pages are ordinary, prepared, refcount-one, uncharged, non-compound
allocations. Only pool ownership uses page->lru. Borrowing removes the list
entry and decrements NR_KERNEL_MISC_RECLAIMABLE under the pool lock. It
never repeats prep_new_page, changes the raw refcount or changes buddy free
statistics. The original page-owner allocation site remains the refill
site. A loan permanently leaves the reserve and follows normal free rules.

Refill removes DIRECT_RECLAIM, COMP and ACCOUNT from
GFP_KERNEL|NORETRY|NOWARN: no direct
reclaim or OOM is requested for refill. Before each allocation, the chosen
zone must exceed its high watermark plus the entire remaining reserve
budget. A failed allocation/watermark check delays further attempts by
300 ms. The post-allocation lock checks enabled, generation, capacity,
zone and pfmemalloc before accepting a page; a rejected page is freed.
Normal free_unref_page completion merely sends a throttled wakeup; it
never gives a page to the reserve.

The NUMA-aware, non-memcg-aware shrinker counts and returns only stored
pages, in base-page units. An order-one block is indivisible, so a scan
can exceed its budget by one base page. Stats pressure_returned and
stop_drain also use base-page units; high/low use blocks. A pressure scan invalidates an in-progress
refill allocation and suppresses refill for at least 300 ms; the high
watermark is checked again thereafter. Detached pages are freed outside
the pool spinlock. No borrowed page is retained or reclaimed by the pool.

Control writes are serialized by a mutex. The shared allocator reserve
arbiter is acquired before startup and retained through complete drain;
C22 must use the same arbiter. Startup unwinds shrinker registration and
ownership in reverse order on failure. Shutdown closes the borrowing and
refill gate under the list lock, unregisters/synchronizes the shrinker,
stops the worker, then drains the list and releases ownership. The pool,
queue and counters are static built-in objects: borrowers finish list and
accounting work before releasing that same lock and never hold a pointer
to destructible pool metadata. Shutdown does not wait for ordinary users
to free pages already handed out. Runtime readers use static metadata;
stats take the pool lock for a coherent snapshot.

Object compilation and host reserve-owner checks do not establish actual
allocator, cpuset migration, memcg rejection, OOM, pressure, concurrent
shutdown or performance behavior. Target-device controlled allocation and
memory-pressure validation is required before enabling this experiment.
