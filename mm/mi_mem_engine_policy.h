/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MI_MEM_ENGINE_POLICY_H
#define _MI_MEM_ENGINE_POLICY_H
#include <linux/errno.h>
#include "../include/uapi/linux/mi_mem_engine.h"
static inline int mme_validate_begin(const struct mme_request *r, __u64 last)
{
	unsigned int i;

	if (r->version != MME_VERSION || r->size != sizeof(*r) ||
	    !r->cookie || !r->lease_ms || r->lease_ms > MME_MAX_LEASE_MS ||
	    !r->nr_tasks || r->nr_tasks > MME_MAX_TASKS)
		return -EINVAL;
	if (r->cookie <= last)
		return -ESTALE;
	for (i = 0; i < MME_MAX_TASKS; i++) {
		if (i < r->nr_tasks) {
			if (r->tasks[i].binder_fd < 0 || !r->tasks[i].binder_handle ||
			    !r->tasks[i].tid)
				return -EINVAL;
		} else if (r->tasks[i].binder_fd || r->tasks[i].binder_handle ||
			   r->tasks[i].tid || r->tasks[i].uid) {
			return -EINVAL;
		}
	}
	return 0;
}
static inline int mme_session_live(__u64 now, __u64 deadline)
{
	return now < deadline;
}
static inline int mme_end_matches(__u64 cookie, __u64 active)
{
	return cookie && cookie == active;
}
static inline unsigned long mme_policy_mark(unsigned long mark, int hit)
{
	return hit ? mark - mark / 4 : mark;
}
#endif
