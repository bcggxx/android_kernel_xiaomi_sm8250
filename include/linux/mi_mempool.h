/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MI_MEMPOOL_H
#define _LINUX_MI_MEMPOOL_H
#include <linux/gfp.h>
struct zone;
#ifdef CONFIG_MI_MEMPOOL
struct page *mi_mempool_borrow(gfp_t gfp, unsigned int order, struct zone *zone);
void mi_mempool_free_event(void);
#else
static inline struct page *mi_mempool_borrow(gfp_t gfp, unsigned int order,
					    struct zone *zone) { return NULL; }
static inline void mi_mempool_free_event(void) { }
#endif
/* Both allocator-reserve implementations must use this single arbiter. */
enum allocator_reserve_owner { RESERVE_NONE, RESERVE_C21, RESERVE_C22 };
int allocator_reserve_acquire(enum allocator_reserve_owner owner);
void allocator_reserve_release(enum allocator_reserve_owner owner);
#endif
