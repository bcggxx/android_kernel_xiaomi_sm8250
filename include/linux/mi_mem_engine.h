/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MI_MEM_ENGINE_H
#define _LINUX_MI_MEM_ENGINE_H
#include <linux/gfp.h>
#ifdef CONFIG_MI_MEM_ENGINE
unsigned long mme_allocation_mark(unsigned long mark, gfp_t gfp,
				 unsigned int order, bool privileged);
#else
static inline unsigned long mme_allocation_mark(unsigned long mark, gfp_t gfp,
					unsigned int order, bool privileged)
{
	return mark;
}
#endif
#endif
