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

When built in, the worker accepts requests by default. Write a boolean to
/sys/module/kshrink_slabd/parameters/enabled to enable or disable new requests.
Disabling does not cancel an accepted or running request.

See Documentation/ABI/testing/sysfs-module-kshrink-slabd for the interface.
