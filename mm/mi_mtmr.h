/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MI_MTMR_H
#define _MM_MI_MTMR_H
struct page;
struct list_head;
struct pglist_data;
#if defined(CONFIG_MI_ASYNC_RECLAIM) || defined(CONFIG_MI_MTMR)
unsigned long mi_reclaim_file_page(struct list_head *pages,
		struct pglist_data *pgdat, bool unmapped_only);
#endif
#ifdef CONFIG_MI_MTMR
void mi_mtmr_ra_prepare(struct page *page, void *producer);
void mi_mtmr_ra_inserted(struct page *page);
void mi_mtmr_ra_complete(void *producer);
void mi_mtmr_nomap(struct page *page);
void mi_mtmr_remap(struct page *page);
#else
static inline void mi_mtmr_ra_prepare(struct page *page, void *producer) { }
static inline void mi_mtmr_ra_inserted(struct page *page) { }
static inline void mi_mtmr_ra_complete(void *producer) { }
static inline void mi_mtmr_nomap(struct page *page) { }
static inline void mi_mtmr_remap(struct page *page) { }
#endif
#endif
