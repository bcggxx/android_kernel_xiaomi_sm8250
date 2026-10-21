// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/resource.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <trace/hooks/binder.h>
#include <uapi/linux/android/binder.h>
#include <uapi/linux/sched/types.h>

#include "../../android/binder_internal.h"

static bool binder_prio_is_rt(unsigned int policy)
{
	return policy == SCHED_FIFO || policy == SCHED_RR;
}

/* Caller holds RCU while inspecting task group leaders. */
static bool binder_prio_is_graphics_sender(struct task_struct *sender,
					   struct task_struct *receiver)
{
	char group[TASK_COMM_LEN], name[TASK_COMM_LEN], target[TASK_COMM_LEN];

	if (!binder_prio_is_rt(READ_ONCE(sender->policy)))
		return false;

	get_task_comm(group, sender->group_leader);
	get_task_comm(name, sender);
	get_task_comm(target, receiver->group_leader);

	if (!strcmp(group, "cameraserver"))
		return true;
	if (!strcmp(group, "com.miui.home") &&
	    !strcmp(name, "RenderThread") && !strcmp(target, "surfaceflinger"))
		return true;
	if (!strcmp(group, "surfaceflinger") && !strcmp(name, "passBlur"))
		return true;
	if (sender->pid != sender->tgid)
		return false;

	return !strcmp(name, "com.miui.home") ||
		!strcmp(name, "ndroid.systemui") || !strcmp(name, "surfaceflinger");
}

static bool binder_prio_is_graphics_transaction(struct binder_transaction *t,
						struct task_struct *receiver)
{
	struct task_struct *sender = NULL;
	bool graphics = false;

	if (!t || !receiver || (t->flags & TF_ONE_WAY))
		return false;

	/* t->from can disappear concurrently with sender thread teardown. */
	rcu_read_lock();
	spin_lock(&t->lock);
	if (t->from && t->from->task) {
		sender = t->from->task;
		get_task_struct(sender);
	}
	spin_unlock(&t->lock);
	if (sender)
		graphics = binder_prio_is_graphics_sender(sender, receiver);
	rcu_read_unlock();
	if (sender)
		put_task_struct(sender);
	return graphics;
}

static bool binder_prio_receiver_allows_rt(struct task_struct *task)
{
	return has_capability_noaudit(task, CAP_SYS_NICE) ||
		task_rlimit(task, RLIMIT_RTPRIO) >= 1;
}

static void binder_prio_set_priority(void *unused, struct binder_transaction *t,
				     struct task_struct *task)
{
	struct sched_param params = { .sched_priority = 1 };

	if (!binder_prio_is_graphics_transaction(t, task))
		return;
	/* Preserve a stronger/equal RT priority and Binder's receiver limits. */
	if (READ_ONCE(task->normal_prio) <= MAX_RT_PRIO - 2 ||
	    !binder_prio_receiver_allows_rt(task))
		return;

	/* Binder saved the old priority before this hook and restores it later. */
	sched_setscheduler_nocheck(task, SCHED_FIFO | SCHED_RESET_ON_FORK, &params);
}

static int __init binder_prio_init(void)
{
	int ret;

	ret = register_trace_android_vh_binder_set_priority(binder_prio_set_priority,
							    NULL);
	if (ret)
		return ret;
	pr_info("binder_prio: transaction-scoped graphics priority enabled\n");
	return 0;
}

static void __exit binder_prio_exit(void)
{
	unregister_trace_android_vh_binder_set_priority(binder_prio_set_priority,
							NULL);
	tracepoint_synchronize_unregister();
}

module_init(binder_prio_init);
module_exit(binder_prio_exit);
MODULE_LICENSE("GPL");
