.. SPDX-License-Identifier: GPL-2.0

===============================
Moon direct-reclaim RT bypass
===============================

The recovered ``moon_mmap_opt`` Kconfig declares two independent intents:

* tune DMA-IOMMU high-order allocations; and
* allow real-time tasks to bypass direct-reclaim throttling.

This kernel implementation covers only the second, bounded behavior.
``CONFIG_MOON_MMAP_OPT`` is disabled by default. When enabled, a current task
using ``SCHED_FIFO``, ``SCHED_RR`` or ``SCHED_DEADLINE`` skips the pfmemalloc
wait in ``throttle_direct_reclaim()``. The task still enters the normal direct
reclaim path and remains subject to allocation eligibility, fatal-signal,
kernel-thread and reclaim-progress behavior.

The option does not tune DMA-IOMMU allocation order, change generic mmap
entry points, borrow C22 QoS state or make non-real-time tasks bypass the
throttle. Those behaviors require separate source evidence and validation.

Enabling this option can change memory-pressure latency and reserve-consumption
behavior. It requires target-device testing with real-time workloads, direct
reclaim pressure, OOM/fatal-signal handling and concurrency coverage before it
can be treated as runtime accepted.
