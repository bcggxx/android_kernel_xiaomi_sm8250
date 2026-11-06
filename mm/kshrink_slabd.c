// SPDX-License-Identifier: GPL-2.0-only
/*
 * Optional background slab reclaim for single-node Android systems.
 *
 * Unlike vendor imports, requests own their memcg reference and never replace
 * the submitting task's reclaim_state. Direct reclaim and drop_caches stay
 * synchronous. A busy worker or increasing pressure also falls back to sync.
 */
#include <linux/cgroup.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/vmstat.h>
#include <linux/memcontrol.h>
#include <linux/mmzone.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/wait.h>

#include "internal.h"

struct slabd_request {
	struct mem_cgroup *memcg;
	gfp_t gfp_mask;
	int nid;
	int priority;
	bool policy;
};

static DEFINE_SPINLOCK(slabd_lock);
static DECLARE_WAIT_QUEUE_HEAD(slabd_wait);
static DECLARE_WAIT_QUEUE_HEAD(slabd_idle);
static struct task_struct *slabd_task;
static struct slabd_request slabd_request;
static bool slabd_pending;
static bool slabd_busy;
static bool slabd_enabled __read_mostly;
static bool slabd_global_scan __read_mostly;

/* All policy state and statistics use slabd_lock; control never nests in it. */
static DEFINE_MUTEX(slabd_control);
static bool slabd_policy_enabled;
static bool slabd_launch;
static bool slabd_root_hint;
static bool slabd_force;
static bool slabd_scanned;
static bool slabd_deadline_valid;
static unsigned long slabd_deadline;
static unsigned long slabd_last_force;
static unsigned int slabd_backoff;
static unsigned int slabd_sleep_ms = 30000;
static unsigned int slabd_max_sleep_ms = 30000;
static unsigned int slabd_priority = DEF_PRIORITY;
static unsigned int slabd_pending_priority = DEF_PRIORITY;
static unsigned int slabd_high_mb = 2048;
static unsigned int slabd_low_mb = 50;
static unsigned long slabd_transferred, slabd_fallbacks, slabd_hints;
static unsigned long slabd_periodic, slabd_skips, slabd_recalculations;
static unsigned long slabd_scans, slabd_objects, slabd_force_wakes;
static unsigned long slabd_launch_started, slabd_launch_ended;
static unsigned int slabd_iterator_live;
static unsigned long slabd_control_epoch;

static bool slabd_policy_active(void)
{
	return slabd_enabled && slabd_global_scan && slabd_policy_enabled;
}

/* Inputs are KiB and reclaimed objects, never pages disguised as objects. */
static unsigned int slabd_policy_delay(u64 slab, u64 low, u64 high,
		unsigned int maximum, bool no_root, unsigned long reclaimed,
		unsigned int *counter)
{
	u64 delay = no_root ? 6000 : 1000;

	if (slab <= low)
		delay += 9000;
	else if (slab < high)
		delay += div64_u64(9000 * (high - slab), high - low);
	if (no_root) {
		*counter = min(*counter + 1, 5U);
		delay += 5000 * *counter;
	} else if (reclaimed >= 10000) {
		*counter = 0;
	} else {
		*counter = min(*counter + 1, 5U);
		delay += (reclaimed ? 2000 : 5000) * (*counter + 1);
	}
	return clamp_t(unsigned int, delay, 1000, maximum);
}

static void slabd_set_deadline(void)
{
	slabd_deadline_valid = slabd_policy_active();
	slabd_deadline = jiffies + msecs_to_jiffies(slabd_sleep_ms);
}

static void slabd_end_launch(void)
{
	if (slabd_launch) {
		slabd_launch = false;
		slabd_launch_ended++;
	}
}

static void slabd_launch_timeout(struct timer_list *timer)
{
	unsigned long flags;

	spin_lock_irqsave(&slabd_lock, flags);
	slabd_end_launch();
	spin_unlock_irqrestore(&slabd_lock, flags);
	wake_up(&slabd_wait);
}
static DEFINE_TIMER(slabd_launch_timer, slabd_launch_timeout);

static bool slabd_is_idle(void)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&slabd_lock, flags);
	idle = !slabd_pending && !slabd_busy;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return idle;
}

static int slabd_set_enabled(const char *val, const struct kernel_param *kp)
{
	bool enabled;
	struct kernel_param parsed = { .arg = &enabled };
	unsigned long flags;
	int ret;

	ret = param_set_bool(val, &parsed);
	if (ret)
		return ret;

	mutex_lock(&slabd_control);
	spin_lock_irqsave(&slabd_lock, flags);
	if (enabled && !slabd_enabled) {
		slabd_root_hint = true;
		slabd_scanned = false;
		slabd_backoff = 0;
		slabd_sleep_ms = slabd_max_sleep_ms;
		slabd_pending_priority = slabd_priority;
	}
	slabd_control_epoch++;
	WRITE_ONCE(slabd_enabled, enabled);
	if (!enabled) {
		slabd_deadline_valid = false;
		slabd_force = false;
		slabd_root_hint = false;
		if (slabd_global_scan)
			WRITE_ONCE(slabd_pending, false);
		slabd_end_launch();
	} else if (!slabd_deadline_valid) {
		slabd_set_deadline();
	}
	spin_unlock_irqrestore(&slabd_lock, flags);
	if (!enabled)
		del_timer_sync(&slabd_launch_timer);
	wake_up(&slabd_wait);
	/* No worker takes the control or parameter mutex. */
	ret = enabled ? 0 :
		wait_event_freezable(slabd_idle, slabd_is_idle());
	mutex_unlock(&slabd_control);
	return ret;
}

static const struct kernel_param_ops slabd_enabled_ops = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = slabd_set_enabled,
	.get = param_get_bool,
};
module_param_cb(enabled, &slabd_enabled_ops, &slabd_enabled, 0644);
MODULE_PARM_DESC(enabled, "Allow background slab reclaim requests");

/* Mode changes cannot reinterpret a pending or running request. */
static int slabd_set_global_scan(const char *val, const struct kernel_param *kp)
{
	bool global_scan;
	struct kernel_param parsed = { .arg = &global_scan };
	unsigned long flags;
	int ret;

	ret = param_set_bool(val, &parsed);
	if (ret)
		return ret;
	spin_lock_irqsave(&slabd_lock, flags);
	if (slabd_enabled || slabd_pending || slabd_busy)
		ret = -EBUSY;
	else
		WRITE_ONCE(slabd_global_scan, global_scan);
	spin_unlock_irqrestore(&slabd_lock, flags);
	return ret;
}

static const struct kernel_param_ops slabd_global_scan_ops = {
	.set = slabd_set_global_scan,
	.get = param_get_bool,
};
module_param_cb(global_scan, &slabd_global_scan_ops, &slabd_global_scan, 0644);
MODULE_PARM_DESC(global_scan,
		 "Scan all memcgs; change only while disabled and idle");

static int slabd_set_policy(const char *val, const struct kernel_param *kp)
{
	bool enabled;
	struct kernel_param parsed = { .arg = &enabled };
	unsigned long flags;
	int ret = param_set_bool(val, &parsed);

	if (ret)
		return ret;
	spin_lock_irqsave(&slabd_lock, flags);
	if (slabd_enabled || slabd_pending || slabd_busy)
		ret = -EBUSY;
	else
		slabd_policy_enabled = enabled;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return ret;
}

static const struct kernel_param_ops slabd_policy_ops = {
	.set = slabd_set_policy,
	.get = param_get_bool,
};
module_param_cb(policy, &slabd_policy_ops, &slabd_policy_enabled, 0644);
MODULE_PARM_DESC(policy, "Adaptive global policy; change while disabled/idle");

static int slabd_set_launch(const char *val, const struct kernel_param *kp)
{
	unsigned int value;
	unsigned long flags;
	bool same;
	int ret = kstrtouint(val, 0, &value);

	if (ret || value > 1)
		return ret ? ret : -EINVAL;
	mutex_lock(&slabd_control);
	spin_lock_irqsave(&slabd_lock, flags);
	same = slabd_launch == value;
	if (!slabd_policy_active())
		ret = -EINVAL;
	spin_unlock_irqrestore(&slabd_lock, flags);
	if (ret || same)
		goto out;
	/* Serialize cancel before a new session: an old callback cannot end it. */
	del_timer_sync(&slabd_launch_timer);
	spin_lock_irqsave(&slabd_lock, flags);
	if (value) {
		slabd_launch = true;
		slabd_launch_started++;
		mod_timer(&slabd_launch_timer,
			  jiffies + msecs_to_jiffies(3000));
	} else {
		slabd_end_launch();
	}
	spin_unlock_irqrestore(&slabd_lock, flags);
	wake_up(&slabd_wait);
out:
	mutex_unlock(&slabd_control);
	return ret;
}

static const struct kernel_param_ops slabd_launch_ops = {
	.set = slabd_set_launch,
	.get = param_get_bool,
};
module_param_cb(app_launch_scenario, &slabd_launch_ops, &slabd_launch, 0644);

static int slabd_set_uint(const char *val, const struct kernel_param *kp)
{
	unsigned int value, *target = kp->arg;
	unsigned long flags;
	int ret = kstrtouint(val, 0, &value);

	if (ret)
		return ret;
	mutex_lock(&slabd_control);
	spin_lock_irqsave(&slabd_lock, flags);
	if ((target == &slabd_sleep_ms && (value < 1000 || value > 30000)) ||
	    (target == &slabd_max_sleep_ms &&
	     (value < 1000 || value > 120000)) ||
	    (target == &slabd_priority && value > DEF_PRIORITY) ||
	    (target == &slabd_high_mb &&
	     (value > 2048 || value <= slabd_low_mb)) ||
	    (target == &slabd_low_mb &&
	     (value < 50 || value >= slabd_high_mb))) {
		ret = -EINVAL;
	} else {
		*target = value;
		if (target == &slabd_priority)
			slabd_pending_priority = min(slabd_pending_priority, value);
		slabd_sleep_ms = min(slabd_sleep_ms, slabd_max_sleep_ms);
	}
	spin_unlock_irqrestore(&slabd_lock, flags);
	mutex_unlock(&slabd_control);
	return ret;
}
static const struct kernel_param_ops slabd_uint_ops = {
	.set = slabd_set_uint,
	.get = param_get_uint,
};
module_param_cb(sleep_time, &slabd_uint_ops, &slabd_sleep_ms, 0644);
module_param_cb(max_sleep_time, &slabd_uint_ops, &slabd_max_sleep_ms, 0644);
module_param_cb(priority, &slabd_uint_ops, &slabd_priority, 0644);
module_param_cb(slab_pressure_high_mb, &slabd_uint_ops, &slabd_high_mb, 0644);
module_param_cb(slab_pressure_low_mb, &slabd_uint_ops, &slabd_low_mb, 0644);

static int slabd_get_stats(char *buffer, const struct kernel_param *kp)
{
	unsigned long flags;
	int length;

	spin_lock_irqsave(&slabd_lock, flags);
	length = scnprintf(buffer, PAGE_SIZE,
		"transferred=%lu fallback=%lu hints=%lu periodic=%lu "
		"recalculations=%lu scans=%lu objects=%lu skips=%lu "
		"force_wakes=%lu launch_started=%lu launch_ended=%lu "
		"pending=%u busy=%u iterator=%u launch=%u timer=%u "
		"sleep_ms=%u backoff=%u\n",
		slabd_transferred, slabd_fallbacks, slabd_hints, slabd_periodic,
		slabd_recalculations, slabd_scans, slabd_objects, slabd_skips,
		slabd_force_wakes, slabd_launch_started, slabd_launch_ended,
		slabd_pending, slabd_busy, slabd_iterator_live, slabd_launch,
		timer_pending(&slabd_launch_timer), slabd_sleep_ms, slabd_backoff);
	spin_unlock_irqrestore(&slabd_lock, flags);
	return length;
}
static const struct kernel_param_ops slabd_stats_ops = {
	.get = slabd_get_stats,
};
module_param_cb(stats, &slabd_stats_ops, NULL, 0444);

/* Below a managed zone's minimum watermark, let synchronous reclaim work. */
static bool slabd_under_pressure(int nid)
{
	struct zone *zone;

	for (zone = NODE_DATA(nid)->node_zones;
	     zone < NODE_DATA(nid)->node_zones + MAX_NR_ZONES; zone++) {
		if (managed_zone(zone) &&
		    !zone_watermark_ok_safe(zone, 0, min_wmark_pages(zone),
					   zone_idx(zone)))
			return true;
	}
	return false;
}

bool wakeup_kshrink_slabd(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority,
			bool global_reclaim)
{
	unsigned long flags;

	bool notify = false;

	if (!current_is_kswapd() || current == READ_ONCE(slabd_task))
		return false;
	spin_lock_irqsave(&slabd_lock, flags);
	if (slabd_policy_active() && global_reclaim &&
	    (!memcg || memcg == root_mem_cgroup)) {
		slabd_root_hint = true;
		slabd_pending_priority = min_t(unsigned int,
			slabd_pending_priority, priority);
		slabd_hints++;
		if (priority <= 2) {
			slabd_force = true;
			if (time_after_eq(jiffies, slabd_last_force +
					  msecs_to_jiffies(2000))) {
				slabd_last_force = jiffies;
				slabd_force_wakes++;
				notify = true;
			}
		}
	}
	/* Hints do not transfer a rejected call's reclaim responsibility. */
	if (priority < DEF_PRIORITY - 2 || !slabd_enabled || !slabd_task ||
	    slabd_pending || slabd_busy || slabd_launch)
		goto synchronous;
	if (slabd_global_scan &&
	    (!global_reclaim || (memcg && memcg != root_mem_cgroup) ||
	     slabd_under_pressure(nid)))
		goto synchronous;
	if (!slabd_global_scan && memcg && !css_tryget_online(&memcg->css))
		goto synchronous;
	slabd_request.memcg = slabd_global_scan ? NULL : memcg;
	slabd_request.gfp_mask = gfp_mask;
	slabd_request.nid = nid;
	slabd_request.priority = priority;
	slabd_request.policy = false;
	slabd_transferred++;
	WRITE_ONCE(slabd_pending, true);
	spin_unlock_irqrestore(&slabd_lock, flags);
	wake_up(&slabd_wait);
	return true;

synchronous:
	slabd_fallbacks++;
	spin_unlock_irqrestore(&slabd_lock, flags);
	if (notify)
		wake_up(&slabd_wait);
	return false;
}

static bool slabd_take_request(struct slabd_request *request)
{
	unsigned long flags;

	spin_lock_irqsave(&slabd_lock, flags);
	/* Disable may have cancelled global pending after the wait succeeded. */
	if (!slabd_pending) {
		spin_unlock_irqrestore(&slabd_lock, flags);
		return false;
	}
	*request = slabd_request;
	if (slabd_policy_active()) {
		if (request->policy)
			request->priority = min_t(unsigned int,
				request->priority, slabd_pending_priority);
		slabd_root_hint = false;
		slabd_pending_priority = slabd_priority;
	}
	WRITE_ONCE(slabd_pending, false);
	WRITE_ONCE(slabd_busy, true);
	spin_unlock_irqrestore(&slabd_lock, flags);
	return true;
}

static unsigned long slabd_scan_global(struct slabd_request *request)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long reclaimed = 0, count, flags;

	/* Runtime-disabled MEMCG has no iterator entries, but global slabs exist. */
	if (mem_cgroup_disabled()) {
		if (READ_ONCE(slabd_enabled) && !kthread_should_stop() &&
		    !slabd_under_pressure(request->nid))
			reclaimed = shrink_slab(request->gfp_mask, request->nid, NULL,
				    request->priority);
		return reclaimed;
	}

	/* iter owns prev: passing it back releases it, including at final NULL. */
	while ((memcg = mem_cgroup_iter(NULL, memcg, NULL))) {
		if (!READ_ONCE(slabd_enabled) || kthread_should_stop() ||
		    slabd_under_pressure(request->nid)) {
			mem_cgroup_iter_break(NULL, memcg);
			break;
		}
		spin_lock_irqsave(&slabd_lock, flags);
		slabd_iterator_live = 1;
		spin_unlock_irqrestore(&slabd_lock, flags);
		count = shrink_slab(request->gfp_mask, request->nid, memcg,
				    request->priority);
		reclaimed += min(count, ULONG_MAX - reclaimed);
		cond_resched();
	}
	spin_lock_irqsave(&slabd_lock, flags);
	slabd_iterator_live = 0;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return reclaimed;
}

/* Runs under the same slot lock as root transfer and disable. */
static void slabd_policy_schedule(void)
{
	u64 slab = (u64)global_node_page_state(NR_SLAB_RECLAIMABLE) <<
		   (PAGE_SHIFT - 10);

	if (!slabd_policy_active() || slabd_pending || slabd_busy ||
	    (!slabd_force && (!slabd_deadline_valid ||
			     time_before(jiffies, slabd_deadline))))
		return;
	if (slabd_force) {
		slabd_backoff = 0;
		slabd_sleep_ms = 1000;
	} else if (slabd_scanned && !slabd_root_hint) {
		slabd_sleep_ms = slabd_policy_delay(slab,
			(u64)slabd_low_mb << 10, (u64)slabd_high_mb << 10,
			slabd_max_sleep_ms, true, 0, &slabd_backoff);
		slabd_recalculations++;
		slabd_set_deadline();
		return;
	}
	/* Fixed node0 is valid for this !NUMA/!MEMORY_HOTPLUG option. */
	slabd_request.memcg = NULL;
	slabd_request.gfp_mask = GFP_KERNEL;
	slabd_request.nid = 0;
	slabd_request.priority = slabd_pending_priority;
	slabd_request.policy = true;
	slabd_periodic++;
	WRITE_ONCE(slabd_pending, true);
	slabd_deadline_valid = false;
}

static long slabd_wait_timeout(void)
{
	unsigned long flags;
	long timeout = MAX_SCHEDULE_TIMEOUT;

	spin_lock_irqsave(&slabd_lock, flags);
	if (slabd_policy_active() && slabd_deadline_valid)
		timeout = time_after(slabd_deadline, jiffies) ?
			slabd_deadline - jiffies : 1;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return timeout;
}

static bool slabd_worker_ready(unsigned long epoch)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&slabd_lock, flags);
	slabd_policy_schedule();
	ready = slabd_pending;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return ready || READ_ONCE(slabd_control_epoch) != epoch ||
		kthread_should_stop();
}

static bool slabd_skip_policy(struct slabd_request *request)
{
	unsigned long flags;
	bool skip;

	spin_lock_irqsave(&slabd_lock, flags);
	skip = request->policy && slabd_launch && !slabd_force;
	if (skip)
		slabd_skips++;
	/* Consumed once, without discarding hints arriving during the scan. */
	if (slabd_force) {
		slabd_backoff = 0;
		slabd_sleep_ms = 1000;
	}
	slabd_force = false;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return skip;
}

static void slabd_finish_request(unsigned long reclaimed, bool skipped)
{
	unsigned long flags;
	u64 slab = (u64)global_node_page_state(NR_SLAB_RECLAIMABLE) <<
		   (PAGE_SHIFT - 10);

	spin_lock_irqsave(&slabd_lock, flags);
	if (slabd_policy_active()) {
		if (!skipped) {
			slabd_scanned = true;
			slabd_scans++;
			slabd_objects += min(reclaimed, ULONG_MAX - slabd_objects);
			slabd_sleep_ms = slabd_policy_delay(slab,
				(u64)slabd_low_mb << 10,
				(u64)slabd_high_mb << 10,
				slabd_max_sleep_ms, false, reclaimed,
				&slabd_backoff);
		} else {
			/* Launch skips retry the policy without marking a scan done. */
			slabd_root_hint = true;
			slabd_pending_priority = min_t(unsigned int,
				slabd_pending_priority, slabd_request.priority);
			slabd_sleep_ms = 1000;
		}
		slabd_set_deadline();
	}
	WRITE_ONCE(slabd_busy, false);
	spin_unlock_irqrestore(&slabd_lock, flags);
	wake_up_all(&slabd_idle);
}

static int kshrink_slabd(void *unused)
{
	struct reclaim_state reclaim_state = { 0 };
	struct slabd_request request;
	unsigned long flags, reclaimed, epoch;
	bool skipped;

	current->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	current->reclaim_state = &reclaim_state;
	set_freezable();

	for (;;) {
		epoch = READ_ONCE(slabd_control_epoch);
		if (wait_event_freezable_timeout(slabd_wait,
			slabd_worker_ready(epoch), slabd_wait_timeout()) < 0)
			continue;
		if (kthread_should_stop())
			break;
		if (!slabd_take_request(&request))
			continue;

		reclaim_state.reclaimed_slab = 0;
		reclaimed = 0;
		skipped = slabd_skip_policy(&request);
		if (!skipped) {
			if (READ_ONCE(slabd_global_scan))
				reclaimed = slabd_scan_global(&request);
			else {
				reclaimed = shrink_slab(request.gfp_mask, request.nid,
					request.memcg, request.priority);
				mem_cgroup_put(request.memcg);
			}
		}
		slabd_finish_request(reclaimed, skipped);
		cond_resched();
	}

	/* No normal stop path exists for this built-in, but keep refs balanced. */
	spin_lock_irqsave(&slabd_lock, flags);
	slabd_task = NULL;
	WRITE_ONCE(slabd_busy, true);
	request.memcg = slabd_pending ? slabd_request.memcg : NULL;
	WRITE_ONCE(slabd_pending, false);
	spin_unlock_irqrestore(&slabd_lock, flags);
	mem_cgroup_put(request.memcg);
	spin_lock_irqsave(&slabd_lock, flags);
	WRITE_ONCE(slabd_busy, false);
	spin_unlock_irqrestore(&slabd_lock, flags);
	wake_up_all(&slabd_idle);
	current->reclaim_state = NULL;
	return 0;
}

static int __init kshrink_slabd_init(void)
{
	struct task_struct *task;
	unsigned long flags;

	/* Express donor 4K-equivalent RAM rounding using the target PAGE_SHIFT. */
	if (slabd_high_mb == 2048 && slabd_low_mb == 50) {
		u64 pages4k = (u64)totalram_pages << (PAGE_SHIFT - 12);

		slabd_high_mb = div64_u64(pages4k, 2560);
		slabd_low_mb = max_t(u64, 50,
			div64_u64(3 * (pages4k >> 8), 100));
		if (slabd_high_mb <= slabd_low_mb)
			slabd_high_mb = slabd_low_mb + 100;
	}
	task = kthread_run(kshrink_slabd, NULL, "kshrink_slabd");
	if (IS_ERR(task)) {
		pr_warn("kshrink_slabd: unavailable, keeping synchronous reclaim\n");
		return 0;
	}
	spin_lock_irqsave(&slabd_lock, flags);
	slabd_task = task;
	spin_unlock_irqrestore(&slabd_lock, flags);
	return 0;
}
late_initcall(kshrink_slabd_init);
