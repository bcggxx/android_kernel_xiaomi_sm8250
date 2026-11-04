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

	/* Parameter writes serialize here via the module parameter mutex. */
	spin_lock_irqsave(&slabd_lock, flags);
	WRITE_ONCE(slabd_enabled, enabled);
	/* Global requests hold no CSS pin and may be cancelled before take. */
	if (!enabled && slabd_global_scan)
		WRITE_ONCE(slabd_pending, false);
	spin_unlock_irqrestore(&slabd_lock, flags);

	/* The worker never takes the parameter mutex, including during reclaim. */
	if (!enabled)
		return wait_event_freezable(slabd_idle, slabd_is_idle());
	return 0;
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

	/* Severe pressure must make progress in the reclaiming task itself. */
	if (!current_is_kswapd() || priority < DEF_PRIORITY - 2)
		return false;

	spin_lock_irqsave(&slabd_lock, flags);
	if (!slabd_enabled || !slabd_task || slabd_pending || slabd_busy)
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
	WRITE_ONCE(slabd_pending, true);
	spin_unlock_irqrestore(&slabd_lock, flags);
	wake_up(&slabd_wait);
	return true;

synchronous:
	spin_unlock_irqrestore(&slabd_lock, flags);
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
	WRITE_ONCE(slabd_pending, false);
	WRITE_ONCE(slabd_busy, true);
	spin_unlock_irqrestore(&slabd_lock, flags);
	return true;
}

static void slabd_scan_global(struct slabd_request *request)
{
	struct mem_cgroup *memcg = NULL;

	/* Runtime-disabled MEMCG has no iterator entries, but global slabs exist. */
	if (mem_cgroup_disabled()) {
		if (READ_ONCE(slabd_enabled) && !kthread_should_stop() &&
		    !slabd_under_pressure(request->nid))
			shrink_slab(request->gfp_mask, request->nid, NULL,
				    request->priority);
		return;
	}

	/* iter owns prev: passing it back releases it, including at final NULL. */
	while ((memcg = mem_cgroup_iter(NULL, memcg, NULL))) {
		if (!READ_ONCE(slabd_enabled) || kthread_should_stop() ||
		    slabd_under_pressure(request->nid)) {
			mem_cgroup_iter_break(NULL, memcg);
			break;
		}
		shrink_slab(request->gfp_mask, request->nid, memcg,
			    request->priority);
		cond_resched();
	}
}

static int kshrink_slabd(void *unused)
{
	struct reclaim_state reclaim_state = { 0 };
	struct slabd_request request;
	unsigned long flags;

	current->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	current->reclaim_state = &reclaim_state;
	set_freezable();

	for (;;) {
		if (wait_event_freezable(slabd_wait,
			READ_ONCE(slabd_pending) || kthread_should_stop()))
			continue;
		if (kthread_should_stop())
			break;

		if (!slabd_take_request(&request))
			continue;

		reclaim_state.reclaimed_slab = 0;
		if (READ_ONCE(slabd_global_scan))
			slabd_scan_global(&request);
		else {
			shrink_slab(request.gfp_mask, request.nid, request.memcg,
				    request.priority);
			mem_cgroup_put(request.memcg);
		}

		spin_lock_irqsave(&slabd_lock, flags);
		WRITE_ONCE(slabd_busy, false);
		spin_unlock_irqrestore(&slabd_lock, flags);
		wake_up_all(&slabd_idle);
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
