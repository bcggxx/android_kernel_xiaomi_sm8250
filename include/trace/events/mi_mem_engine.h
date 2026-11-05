/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mi_mem_engine
#if !defined(_TRACE_MI_MEM_ENGINE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MI_MEM_ENGINE_H
#include <linux/tracepoint.h>
TRACE_EVENT(mme_allocation,
	TP_PROTO(u64 cookie, int tid, unsigned long original,
		 unsigned long effective, bool hit),
	TP_ARGS(cookie, tid, original, effective, hit),
	TP_STRUCT__entry(
		__field(u64, cookie)
		__field(int, tid)
		__field(unsigned long, original)
		__field(unsigned long, effective)
		__field(bool, hit)
	),
	TP_fast_assign(
		__entry->cookie = cookie;
		__entry->tid = tid;
		__entry->original = original;
		__entry->effective = effective;
		__entry->hit = hit;
	),
	TP_printk("cookie=%llu tid=%d mark=%lu effective=%lu hit=%d",
		__entry->cookie, __entry->tid, __entry->original,
		__entry->effective, __entry->hit)
);
#endif
#include <trace/define_trace.h>
