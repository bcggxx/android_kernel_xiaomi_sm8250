.. SPDX-License-Identifier: GPL-2.0

Filemap readahead observation
============================

These events observe existing decisions; they do not implement a policy or
change windows, file_ra_state, page flags, requests, or configuration defaults.
Both events are disabled by default.  There is no new Kconfig or sysctl.

Request event
-------------

``filemap:filemap_readahead_request`` is emitted immediately before a real
readahead call in generic buffered read or filemap mmap fault handling:

====== ================== ===============================================
source call site          demand_start / demand_pages (base pages)
====== ================== ===============================================
0      buffered sync      current index / remaining requested pages
1      buffered async     current index / remaining requested pages
2      mmap sequential    current fault offset / 1
3      mmap readaround    current fault offset / 1
4      mmap async         current fault offset / 1
====== ================== ===============================================

The numeric source definitions are in include/trace/events/filemap.h.
Buffered demand is the page envelope of the remaining iterator request at
that call site, not bytes actually copied, not a promise of completion, and
not EOF-clamped.  mmap demand is always this fault's one page, even when the
existing readahead function is passed ra_pages.  A request event can have no
window event: the called function may return without submitting a candidate.
Random mmap faults and other paths that bypass these calls have no request
event.  Absence of either event does not imply absence of demand I/O.
Explicit force readahead and callers outside these five call sites
can produce window events without a matching request event.

Window event
------------

``filemap:filemap_readahead_window`` retains the original candidate range
before EOF and page-cache filtering in __do_page_cache_readahead().  It is
neither the application's demand nor allocated pages, submitted I/O, or a
completed read.  The new fields describe the current calling task and a
nearby memory availability sample:

* ``cpuset_id`` is u64 cgroup_id(css->cgroup), the kernfs node ID, not the
  controller-local css ID or cgroup's internal ID.
* ``cpuset_root`` is the int cgroup root hierarchy_id.  Together these use
  the same definitions as cgroup_attach_task's dst_id / dst_root fields.
* ``cpuset_state`` is 0 when CONFIG_CPUSETS is absent, 1 when CSS_ONLINE was
  observed and css_is_dying() was false, 2 for offline or dying CSS,
  or 3 when no CSS was found.
  All states except 1 mean unknown identity; their ID is 0 and root is -1.
* ``available_pages`` is the signed long result of si_mem_available(), in
  base pages, without an additional clamp or a pressure threshold.
* ``available_ns`` is monotonic elapsed nanoseconds around that single
  si_mem_available() call.  It includes clock-reading overhead and possible
  preemption/interrupt time, not the RCU lookup, trace recording, or total
  decision cost.  It is not a CPU-time measurement.

Sampling occurs only after trace_filemap_readahead_window_enabled() succeeds.
With the event disabled there is no CSS lookup, clock read or memory sample.
The guard may race trace enable/disable, so an in-flight enabled sample can
finish after disable.  Event filtering happens later and does not avoid this
sampling cost.  Enabling only the request event does not enable sampling.
si_mem_available() can scan nodes/zones; enabled overhead requires target
measurement and is not claimed to be negligible or constant.  The sample
is an estimate, not a globally atomic memory snapshot or a pressure verdict.

RCU and identity limits
-----------------------

The code uses task_css(current, cpuset_cgrp_id) inside rcu_read_lock(), guarded
by CONFIG_CPUSETS.  It takes no global cgroup mutex, constructs no path, stores
no task flag and retains no pointer beyond the RCU section.  In this tree,
task_css_set_check() documents RCU access to task->cgroups.  css_set retains
controller CSS references; CSS destruction waits for an RCU grace period
before css_free_rwork_fn().  init_and_link_css() obtains cgroup_get_live(),
and the CSS free path drops that cgroup reference only after freeing CSS.
The cgroup free path then releases its kernfs node via kernfs_put().  Thus
these pointers remain usable during this read-side critical section; no
additional persistent reference or new fd control plane is needed here.

The CSS_ONLINE bit read via READ_ONCE(css->flags) and css_is_dying() are
only observations at their respective instants.  RCU protects
lifetime, not membership stability or an atomic online/membership snapshot.
Concurrent attach/offline can make an online result immediately stale or
expose an older membership.  It is not a pin, current-role guarantee, or
proof that the task stays in that group through I/O.  Unknown states must
not be interpreted as root, foreground, or background.  This event does not
classify roles.  Calibrate controller and hierarchy IDs externally with
cgroup_attach_task plus the known scenario; do not merge same-name groups
across controllers.  Root remount/destruction and ID lifetime changes require
fresh calibration; IDs are not permanent path identities across boots.
The cpuset observation precedes the availability sample; the pair is not
one atomic cgroup/memory snapshot.

Correlation and validation
--------------------------

Both events contain device/inode and trace metadata includes current PID.
For bounded, controlled single-thread test operations, use same-PID event
order plus device/inode to associate the request and subsequent candidates,
with an explicit operation boundary in the test harness.  A request may
produce zero, one or several candidates.  There is no request cookie or
exported API context propagation.  Nested/reentrant I/O, shared file state,
other callers, lost events, concurrent mutation, inode reuse or insufficient
operation boundaries make associations ambiguous; report them as unknown.
This does not promise general atomic request-to-I/O attribution.  Page-cache
and block I/O evidence still need separate validation and appropriate file
extent filtering.  Paths and user data are not recorded by these events.

Object compilation proves type/configuration integration only.  Target
boot, event formats, source coverage, online/unknown transitions, measured
sampling cost and tracing overhead still need runtime verification.  Host
userspace substitutes cannot validate kernel RCU or concurrent migration.
These observations alone do not complete the phase-1 contract or authorize
window shrinking.
