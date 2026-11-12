Graphics Binder priority policy
===============================

CONFIG_BINDER_PRIO installs a priority hook for selected synchronous Binder
transactions from real-time graphics and camera tasks. It does not reorder
the transaction queue. One-way transactions and transactions without a live
sender are excluded. The target node must opt into real-time inheritance
(``inherit_rt``); the hook never overrides a node that disables it.

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
has CAP_SYS_NICE or RLIMIT_RTPRIO of at least 1. A receiver whose saved
pre-transaction policy was FIFO or RR is preserved. The check uses the saved
policy because Binder core may already have inherited the sender's policy
before invoking this hook. The hook does not change the Binder process
default priority. Binder core saves and restores the receiver's previous
priority as part of transaction processing.

The receiver permission check can leave matching transactions unboosted.
This policy does not provide a latency guarantee or replace application
thread scheduling configuration.

Optional main-thread rules
--------------------------

``binder_prio.extra_main_threads`` is a separate boolean module parameter,
default false. Its runtime control is
``/sys/module/binder_prio/parameters/extra_main_threads`` (root write, 0644).
Invalid boolean input is rejected without changing the setting. When enabled,
exact comm values ``rsonalassistant`` and ``.globallauncher`` join the policy
only on main threads (pid == tgid). The legacy list works with either setting.

These names come from GPL-licensed MiCode/Xiaomi_Kernel_OpenSource commit
``56295df675b92553f83bbf333f88b0c6ee8fa7bc``,
``drivers/staging/binder_prio/binder_prio.c``. They are task comm values, not
expanded Android package names. The added rules share the synchronous RT
sender, node, permission and task-reference checks above. They request FIFO1
with RESET_ON_FORK, without adding persistent task state; a scheduler failure
does not create an extension success state.

Disabling the parameter affects subsequent decisions. A callback already
making its decision can complete; in-flight transactions rely on Binder core
normal, error and idle restoration. Module unload unregisters and synchronizes
the hook and likewise relies on core restoration.

This is a safety adaptation: exact matching replaces source prefixes,
persistent SurfaceFlinger default promotion is omitted, and the existing
cameraserver rule remains broader than the source C3Dev-/-ReqQ rule.
QoS/MVP/SCX binary findings are separate mechanisms. The host classifier and
isolated binderfs checks do not establish matching production ROM requests,
nested/error/exit stress coverage, or performance benefit.
