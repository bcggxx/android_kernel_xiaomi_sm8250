Background slab reclaim
=======================

CONFIG_KSHRINK_SLABD provides a built-in worker for asynchronous slab reclaim
from kswapd. It depends on ARM64 and MEMCG and excludes NUMA and memory
hotplug configurations. The Kconfig option defaults to disabled.

Request ownership and fallback
------------------------------

Only kswapd calls with reclaim priority at least DEF_PRIORITY - 2 can submit
work. A single request slot is protected by a spinlock. In single-request mode
each accepted request holds an online memcg reference until completion. The freezable worker uses
its own reclaim_state; it does not overwrite the submitting task's state.

Direct reclaim, drop_caches, severe pressure and submissions while the worker
is busy or unavailable use synchronous slab reclaim. Moving work out of
kswapd can delay slab reclamation; it does not guarantee lower reclaim
latency or increased memory availability.

Runtime control
---------------

When built in, runtime admission defaults to disabled. Write a boolean to
/sys/module/kshrink_slabd/parameters/enabled to enable or disable requests.
Admission and control share the request spinlock: a request either belongs to
the worker before disable, or is rejected and reclaimed synchronously.

Writing false rejects new requests and waits for any accepted work to become
idle, including releasing its memcg reference. Single-request mode completes
accepted pending requests. Global-scan mode cancels pending requests and stops
a running scan at a group boundary. Only a successful write guarantees that background reclaim is idle on return. A
signal can interrupt the drain wait and fail the write; admission remains
disabled, but accepted work may still be active. Write false again to retry
the drain. Invalid booleans leave the state unchanged. Repeated writes of
true or false are supported.

The drain wait is freezable, as is the worker. During suspend both can remain
frozen until thaw; disable has no bounded completion time. Parameter writes
are serialized by the built-in parameter mutex; the worker does not acquire
that mutex. Reclaim continues on the synchronous paths while admission is
disabled. This built-in has no runtime unload or stop interface.

Global scan
-----------

The global_scan boolean parameter defaults to false (single-request mode).
Change it only with admission disabled and the worker idle; otherwise the
write returns EBUSY. Both modes share one slot and one worker. To select a
global scan, successfully write enabled=0, global_scan=1, then enabled=1.

In global-scan mode only a root/NULL memcg call from global kswapd reclaim
can transfer one slab call to the worker. It saves GFP, node and priority;
it never keeps the submitting scan_control or a bare memcg pointer. Other
memcg calls and busy submissions remain synchronous, even during a global
scan. Asynchronous reclaim is not credited to the submitter's nr_reclaimed.

The worker serially calls shrink_slab for each memcg using the standard
iterator reference protocol. Each group checks disable, stop and pressure;
there is a cond_resched after each shrinker call. Below any managed zone's
minimum watermark, global admission falls back synchronously and running
iteration stops, releasing the current iterator reference. Runtime-disabled
MEMCG instead performs one NULL-memcg global slab call. An in-progress
shrinker cannot be forcibly interrupted: disable waits until it returns.

This implements the recovered donor's single-worker serial global memcg
scan with explicit 4.19 lifetime and synchronous fallback rules. It does not
implement the donor's automatic periodic scheduling, launch/timer protocol,
or adaptive high/low slab pressure sleep policy; these remain separate C11
work (23b). No second slab worker is created.

Host selftests exercise extracted admission/control/iterator functions with
pthread, CSS and iterator substitutes; they do not prove real worker
scheduling,
freezer behavior, memcg offline/delete safety or pressure reclaim progress.

See Documentation/ABI/testing/sysfs-module-kshrink-slabd for the interface.
