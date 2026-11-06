// SPDX-License-Identifier: GPL-2.0
/*
 * Moon direct-reclaim throttle policy.
 *
 * The recovered Moon Kconfig declares one bounded behavior: real-time tasks
 * may bypass direct-reclaim throttling. The DMA-IOMMU high-order intent is
 * intentionally not implemented here because its allocator policy is not
 * defined by the recovered source.
 */

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>

#include "moon_mmap_opt.h"

bool moon_mmap_opt_rt_bypass(struct task_struct *task)
{
	unsigned int policy;

	if (!task)
		return false;

	policy = READ_ONCE(task->policy);
	return policy == SCHED_FIFO || policy == SCHED_RR ||
		policy == SCHED_DEADLINE;
}
