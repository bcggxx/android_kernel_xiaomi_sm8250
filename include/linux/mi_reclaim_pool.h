/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MI_RECLAIM_POOL_H
#define _LINUX_MI_RECLAIM_POOL_H
#include <linux/gfp.h>
struct zone;
#ifdef CONFIG_MI_RECLAIM_POOL
struct page *mi_reclaim_pool_borrow(gfp_t gfp, unsigned int order,
				   struct zone *zone);
void mi_reclaim_pool_free_event(void);
#else
static inline struct page *mi_reclaim_pool_borrow(gfp_t gfp,
		unsigned int order, struct zone *zone) { return NULL; }
static inline void mi_reclaim_pool_free_event(void) { }
#endif
#endif
