/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MOON_MMAP_OPT_H
#define _MM_MOON_MMAP_OPT_H

struct task_struct;

#ifdef CONFIG_MOON_MMAP_OPT
bool moon_mmap_opt_rt_bypass(struct task_struct *task);
#else
static inline bool moon_mmap_opt_rt_bypass(struct task_struct *task)
{
	return false;
}
#endif

#endif /* _MM_MOON_MMAP_OPT_H */
