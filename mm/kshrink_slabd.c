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
static struct task_struct *slabd_task;
static struct slabd_request slabd_request;
static bool slabd_pending;
static bool slabd_busy;
static bool slabd_enabled __read_mostly = true;
module_param_named(enabled, slabd_enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Allow background slab reclaim requests");

bool wakeup_kshrink_slabd(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	unsigned long flags;

	/* Severe pressure must make progress in the reclaiming task itself. */
	if (!READ_ONCE(slabd_enabled) || !current_is_kswapd() ||
	    priority < DEF_PRIORITY - 2)
		return false;

	spin_lock_irqsave(&slabd_lock, flags);
	if (!slabd_task || slabd_pending || slabd_busy)
		goto synchronous;
	if (memcg && !css_tryget_online(&memcg->css))
		goto synchronous;

	slabd_request.memcg = memcg;
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

		spin_lock_irqsave(&slabd_lock, flags);
		request = slabd_request;
		WRITE_ONCE(slabd_pending, false);
		slabd_busy = true;
		spin_unlock_irqrestore(&slabd_lock, flags);

		reclaim_state.reclaimed_slab = 0;
		shrink_slab(request.gfp_mask, request.nid, request.memcg,
			    request.priority);
		mem_cgroup_put(request.memcg);

		spin_lock_irqsave(&slabd_lock, flags);
		slabd_busy = false;
		spin_unlock_irqrestore(&slabd_lock, flags);
		cond_resched();
	}

	/* No normal stop path exists for this built-in, but keep refs balanced. */
	spin_lock_irqsave(&slabd_lock, flags);
	slabd_task = NULL;
	request.memcg = slabd_pending ? slabd_request.memcg : NULL;
	WRITE_ONCE(slabd_pending, false);
	spin_unlock_irqrestore(&slabd_lock, flags);
	mem_cgroup_put(request.memcg);
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
