Background slab reclaim
=======================

CONFIG_KSHRINK_SLABD provides a built-in worker for asynchronous slab reclaim
from kswapd. It depends on ARM64 and MEMCG and excludes NUMA and memory
hotplug configurations. The Kconfig option defaults to disabled.

Request ownership and fallback
------------------------------

Only kswapd calls with reclaim priority at least DEF_PRIORITY - 2 can submit
work. A single request slot is protected by a spinlock. Each accepted request
holds an online memcg reference until completion. The freezable worker uses
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

Writing false rejects new requests and waits for any accepted pending or
running request to complete, including releasing its memcg reference. Only a
successful write guarantees that background reclaim is idle on return. A
signal can interrupt the drain wait and fail the write; admission remains
disabled, but accepted work may still be active. Write false again to retry
the drain. Invalid booleans leave the state unchanged. Repeated writes of
true or false are supported.

The drain wait is freezable, as is the worker. During suspend both can remain
frozen until thaw; disable has no bounded completion time. Parameter writes
are serialized by the built-in parameter mutex; the worker does not acquire
that mutex. Reclaim continues on the synchronous paths while admission is
disabled. This built-in has no runtime unload or stop interface.

The local single-slot worker does not implement the donor's global memcg
scan. Host selftests exercise extracted admission/control functions with
pthread and CSS substitutes; they do not prove real worker scheduling,
freezer behavior, memcg offline/delete safety or pressure reclaim progress.

See Documentation/ABI/testing/sysfs-module-kshrink-slabd for the interface.
