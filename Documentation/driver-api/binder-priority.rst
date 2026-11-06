Graphics Binder priority policy
===============================

CONFIG_BINDER_PRIO installs a priority hook for selected synchronous Binder
transactions from real-time graphics and camera tasks. It does not reorder
the transaction queue. One-way transactions and transactions without a live
sender are excluded.

Sender matching
---------------

Task names are compared using the kernel task comm representation, including
its length limit. The policy matches real-time callers as follows:

* Threads in the cameraserver process.
* com.miui.home RenderThread calls to surfaceflinger.
* The passBlur thread in surfaceflinger.
* Main threads named com.miui.home, ndroid.systemui or surfaceflinger.

The sender reference is acquired under the transaction lock. Group-leader
inspection is protected by RCU.

Receiver limits and restoration
-------------------------------

A matching transaction requests SCHED_FIFO priority 1 only when the receiver
has CAP_SYS_NICE or RLIMIT_RTPRIO of at least 1. An observed equal or stronger
real-time priority is preserved. The hook does not change the Binder process
default priority. Binder core saves and restores the receiver's previous
priority as part of transaction processing.

The receiver permission check can leave matching transactions unboosted.
This policy does not provide a latency guarantee or replace application
thread scheduling configuration.
