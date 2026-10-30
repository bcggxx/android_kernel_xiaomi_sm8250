/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_MI_RMAP_EFFICIENCY_H
#define _MM_MI_RMAP_EFFICIENCY_H

struct page;

enum mi_rmap_control {
	MI_RMAP_RECLAIM,
	MI_RMAP_KEEP,
	MI_RMAP_ACTIVATE,
};

#ifdef CONFIG_MI_RMAP_EFFICIENCY
enum mi_rmap_control mi_rmap_check(struct page *page, unsigned long nr_scanned,
				 int priority, unsigned long *nr_protected);
#else
static inline enum mi_rmap_control mi_rmap_check(struct page *page,
		unsigned long nr_scanned, int priority, unsigned long *nr_protected)
{
	return MI_RMAP_RECLAIM;
}
#endif
#endif
