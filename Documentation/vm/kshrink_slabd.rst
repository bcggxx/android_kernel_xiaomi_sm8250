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

Adaptive global policy
----------------------

The policy parameter is a separate boolean, default false. Like global_scan,
change it only while disabled and idle. It has an effect only when both
policy and global_scan are true. The existing worker uses a deadline wait;
there is no second worker or periodic timer. The first deadline after enable
is max_sleep_time (default 30000 ms). Every timeout competes for the same
pending/running slot as transferred calls. A root hint during an occupied
slot does not claim the caller's work; that caller reclaims synchronously.

A policy request serially scans real memcgs through shrink_slab. After a
scan, a timeout with no new root hint only recalculates the next deadline:
it never iterates memcgs. Root hints merge the minimum pending priority.
Priorities below DEF_PRIORITY - 2 still reclaim synchronously. A root
priority <= 2 additionally requests a force round; extra wake notifications
are rate limited to 2000 ms. Force is only a scheduling hint, never a bypass
of synchronous reclaim or the minimum-watermark checks. Pending, force and
control changes can interrupt the target worker's wait. This deliberately
improves on the donor wait loop, whose stop-only predicate did not establish
an immediate force wake despite issuing wake_up.

Slab pressure uses NR_SLAB_RECLAIMABLE and PAGE_SHIFT to obtain KiB. Default
high/low thresholds preserve the donor's rounding through equivalent 4K RAM
pages: high=floor(pages4k/2560) MiB, low=max(50,
floor(3*(pages4k>>8)/100)) MiB; if high<=low, high=low+100. The ordinary base
sleep interpolates from 1000 ms at/above high to 10000 ms at/below low.
The no-new-root branch uses 6000..15000 ms instead. That branch increments
a counter capped at five and adds 5000*counter. After a scan, >=10000
reclaimed objects reset the counter without added delay; otherwise the
counter increments (cap five), and zero objects add 5000*(counter+1), while
small nonzero reclaim adds 2000*(counter+1). Results use the complete target
unsigned long return value and saturating accumulation, rather than the
donor's low-32-bit truncation. Every delay is clamped to 1000..max_sleep_time.

sleep_time accepts 1000..30000 ms and changes a future interval without
interrupting an existing wait. max_sleep_time accepts 1000..120000 ms.
priority accepts 0..12 and cannot raise an already lower pending priority.
slab_pressure_high_mb accepts values <=2048 and strictly above low;
slab_pressure_low_mb accepts values >=50 and strictly below high. Invalid
writes leave the configuration unchanged. High/low validation and snapshot
use the worker state lock, avoiding zero denominators during concurrent
control/reclaim. Mode, policy and admission controls use the parameter
mutex; launch/disable/configuration also use an explicit control mutex.

Launch sessions and shutdown
---------------------------

app_launch_scenario is an administrator input, not an automatic ROM launch
detector. It accepts only 0/1 while global policy is enabled. A 0->1 starts
one 3000 ms session; repeated 1 does not extend it. Explicit 0 or timer
expiry ends that session once. Control writes synchronously cancel an old
timer before creating a new session, so an old callback cannot terminate a
new session. No unknown DR or task-field interface is connected.

Only policy requests may skip scanning during launch, and a force hint
allows a policy round despite launch. An already transferred 23a request
is always fulfilled subject to its normal disable/pressure boundary.
New transfer attempts during launch fall back synchronously. A launch skip
retries policy after 1000 ms; expiry/0 restores eligibility. The statistics
separate transferred calls, synchronous fallbacks, hints, policy requests,
no-root recalculations, scan rounds, reclaimed objects and launch skips.
Scan rounds include pressure-aborted attempts, not a guarantee of reclaim.

Disable first rejects admission and invalidates the deadline under the
state lock, clears pending global work/force hints and ends the session;
then synchronously cancels the launch timer, wakes the worker and waits for
pending/busy to drain. Iterator ownership ends before busy clears. A
successful disable therefore leaves no timer, pending work, active shrinker
or iterator. Statistics remain readable for checking this boundary; a
signal-interrupted write still requires a successful retry as above.
The permanent built-in worker then waits indefinitely; there is no unload
or memory-free path requiring join. Any future destruction path must also
join the worker before freeing its state.

The policy defaults off, including all launch behavior. No enabling,
performance benefit, real-device lifetime safety or ROM automatic launch
integration follows from the host tests. C11 nomap/VIP/DR work remains
separate.

Host selftests exercise extracted production policy/admission/control/iterator
functions with CSS, timers and iterator substitutes; they do not prove real worker
scheduling,
freezer behavior, memcg offline/delete safety or pressure reclaim progress.

See Documentation/ABI/testing/sysfs-module-kshrink-slabd for the interface.
