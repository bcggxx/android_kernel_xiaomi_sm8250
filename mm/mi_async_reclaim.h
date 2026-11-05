/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MI_ASYNC_RECLAIM_H
#define _MM_MI_ASYNC_RECLAIM_H

struct page;
struct list_head;
struct pglist_data;

#ifdef CONFIG_MI_ASYNC_RECLAIM
bool mi_async_reclaim_ready(void);
bool mi_async_reclaim_queue(struct page *page);
#else
static inline bool mi_async_reclaim_ready(void) { return false; }
static inline bool mi_async_reclaim_queue(struct page *page) { return false; }
#endif
#endif
