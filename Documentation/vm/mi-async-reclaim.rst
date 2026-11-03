File-rmap contention handoff (experimental)
==========================================

CONFIG_MI_ASYNC_RECLAIM is a built-in, default-n option. Runtime admission is
also disabled initially. Root may write 1 or 0 to
/sys/kernel/mi_async_reclaim/enabled. The state file reports cumulative base
pages accepted, pending, running, reclaimed and returned, plus capacity.
Accepted equals pending + running + reclaimed + returned. These counters do
not measure performance or synchronous caller reclaim success.

Only normal global kswapd inactive-file reclaim, order zero, with filesystem
and IO permission and without DMA constraints can defer a page. File rmap
reference checks and unmap try the mapping's i_mmap_rwsem once. Only failure
of that file lock is eligible for handoff. Ordinary page lock contention,
anonymous/KSM pages, compound heads, forced reclaim and target-VMA reclaim
retain synchronous behavior. Existing high-mapcount protection runs first.
The worker performs normal reference checking and blocking rmap walks; it
cannot recursively defer its pages. No scan_control or memcg pointer crosses
threads. Compound pages are deliberately excluded to preserve their ordinary
split and base-page accounting without an unlocked split race.

A successful admission transfers the original isolated reference, without
get_page/put_page duplication. Pending plus running is strictly bounded to
256 base pages (one MiB with the target's 4 KiB pages). Rejection leaves the
page on its original putback path. The original node NR_ISOLATED_FILE charge
stays outstanding until worker reclaim or putback terminates it exactly once.
This preserves isolation-pressure throttling. The worker processes one base
page at a time, using fresh GFP_KERNEL scan control and reclaim state. Pages
are returned with putback_lru_page, which follows their current memcg.

Disable is serialized against enable and other disable calls. It closes
admission at the queue lock, cancels pending pages to their normal LRU and
joins the worker after its current page completes. A producer that already
attempted the file trylock can still finish its synchronous putback, but
cannot transfer ownership after closure. No producer retains the worker or
control object. A blocking running rmap walk or filesystem IO can delay
disable; forcibly aborting it would violate the normal reclaim contract.
Signals may interrupt acquisition of the control mutex; after closure starts,
cleanup completes before returning. Empty-queue stop wakes the worker too.

The worker waits interruptibly with explicit stop/freezing predicates and
uses kthread_freezable_should_stop, whose 4.19 refrigerator checks stop.
The one-time set_freezable initialization uses a startup completion before
enable is published, preventing stop from racing its initial try_to_freeze.
Only the control mutex is held across startup/join, never the queue lock.
There is no module unload path: the option and sysfs controls are built-in;
the worker exists only while enabled. Failed thread creation leaves admission
closed. Failed sysfs-group creation drops its kobject; no worker has started.

Provenance and validation
-------------------------

Behavior is adapted from MiCode/Xiaomi_Kernel_OpenSource commit
56295df675b92553f83bbf333f88b0c6ee8fa7bc,
drivers/staging/mi_async_reclaim/mi_async_reclaim.c, original module author
maminghui5 <maminghui5@xiaomi.com>, declaring GPL. The source file has no SPDX
or copyright declaration to copy. This 4.19 adaptation replaces donor raw
page flag indices and hook state with explicit stack-local results, fixes
capacity/wakeup/stop ownership, and intentionally limits eligible contexts.
It does not establish equivalence to any screenshot-only module directory.

The host selftest extracts actual admission/take/completion functions and
checks bounded ownership transitions. It cannot prove kernel rmap locking,
LRU behavior, freezer scheduling or real reclaim. Required device validation
remains: real inactive reclaim under file i_mmap_rwsem contention, observed
handoff and terminal reclaim/putback, full/closed fallback, disable and empty
stop, with reference and per-node isolated-charge conservation. No device
validation or performance benefit is claimed by the host regression.
