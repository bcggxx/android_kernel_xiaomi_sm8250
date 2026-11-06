/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MI_RECLAIM_POOL_H
#define _LINUX_MI_RECLAIM_POOL_H
#include <linux/gfp.h>
struct zone;
#ifdef CONFIG_MI_RECLAIM_POOL
struct page *mi_reclaim_pool_borrow(gfp_t gfp, unsigned int order,
				    struct zone *zone, bool *needs_prep);
void mi_reclaim_pool_free_event(void);
bool mi_reclaim_pool_raw_capture_enabled(void);
bool mi_reclaim_pool_capture_free_page(struct page *page, int migratetype,
				       bool on_list);
void mi_reclaim_pool_return_raw_page(struct page *page);
#else
static inline struct page *
mi_reclaim_pool_borrow(gfp_t gfp, unsigned int order,
		       struct zone *zone, bool *needs_prep)
{
	if (needs_prep)
		*needs_prep = false;
	return NULL;
}

static inline void mi_reclaim_pool_free_event(void) { }

static inline bool mi_reclaim_pool_raw_capture_enabled(void)
{
	return false;
}

static inline bool mi_reclaim_pool_capture_free_page(struct page *page,
						     int migratetype,
						     bool on_list)
{
	return false;
}
#endif
#endif
