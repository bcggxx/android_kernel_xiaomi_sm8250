/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM filemap

#if !defined(_TRACE_FILEMAP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FILEMAP_H

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <linux/mm.h>
#include <linux/memcontrol.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/errseq.h>

DECLARE_EVENT_CLASS(mm_filemap_op_page_cache,

	TP_PROTO(struct page *page),

	TP_ARGS(page),

	TP_STRUCT__entry(
		__field(unsigned long, pfn)
		__field(unsigned long, i_ino)
		__field(unsigned long, index)
		__field(dev_t, s_dev)
	),

	TP_fast_assign(
		__entry->pfn = page_to_pfn(page);
		__entry->i_ino = page->mapping->host->i_ino;
		__entry->index = page->index;
		if (page->mapping->host->i_sb)
			__entry->s_dev = page->mapping->host->i_sb->s_dev;
		else
			__entry->s_dev = page->mapping->host->i_rdev;
	),

	TP_printk("dev %d:%d ino %lx page=%p pfn=%lu ofs=%lu",
		MAJOR(__entry->s_dev), MINOR(__entry->s_dev),
		__entry->i_ino,
		pfn_to_page(__entry->pfn),
		__entry->pfn,
		__entry->index << PAGE_SHIFT)
);

DEFINE_EVENT(mm_filemap_op_page_cache, mm_filemap_delete_from_page_cache,
	TP_PROTO(struct page *page),
	TP_ARGS(page)
	);

DEFINE_EVENT(mm_filemap_op_page_cache, mm_filemap_add_to_page_cache,
	TP_PROTO(struct page *page),
	TP_ARGS(page)
	);

/* Request sources describe call sites, not the eventual I/O submitted. */
#ifndef FILEMAP_RA_SOURCE_DEFINED
#define FILEMAP_RA_SOURCE_DEFINED
enum filemap_readahead_source {
	FILEMAP_RA_READ_SYNC = 0,
	FILEMAP_RA_READ_ASYNC,
	FILEMAP_RA_MMAP_SEQ,
	FILEMAP_RA_MMAP_READAROUND,
	FILEMAP_RA_MMAP_ASYNC,
};
#endif

TRACE_EVENT(filemap_readahead_request,
	TP_PROTO(struct address_space *mapping, unsigned int source,
		 pgoff_t demand_start, unsigned long demand_pages),

	TP_ARGS(mapping, source, demand_start, demand_pages),

	TP_STRUCT__entry(
		__field(unsigned long, i_ino)
		__field(dev_t, s_dev)
		__field(unsigned int, source)
		__field(pgoff_t, demand_start)
		__field(unsigned long, demand_pages)
	),

	TP_fast_assign(
		__entry->i_ino = mapping->host->i_ino;
		__entry->s_dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->source = source;
		__entry->demand_start = demand_start;
		__entry->demand_pages = demand_pages;
	),

	TP_printk("dev=%u:%u ino=0x%lx source=%u demand_start=%lu demand_pages=%lu",
		  MAJOR(__entry->s_dev), MINOR(__entry->s_dev), __entry->i_ino,
		  __entry->source, (unsigned long)__entry->demand_start,
		  __entry->demand_pages)
);

TRACE_EVENT(filemap_readahead_window,
	TP_PROTO(struct address_space *mapping, pgoff_t start_index,
		 unsigned long nr_pages, unsigned long lookahead_pages,
		 u64 cpuset_id, int cpuset_root, unsigned int cpuset_state,
		 long available_pages, u64 available_ns),

	TP_ARGS(mapping, start_index, nr_pages, lookahead_pages, cpuset_id,
		cpuset_root, cpuset_state, available_pages, available_ns),

	TP_STRUCT__entry(
		__field(unsigned long, i_ino)
		__field(dev_t, s_dev)
		__field(pgoff_t, start_index)
		__field(unsigned long, nr_pages)
		__field(unsigned long, lookahead_pages)
		__field(u64, cpuset_id)
		__field(int, cpuset_root)
		__field(unsigned int, cpuset_state)
		__field(long, available_pages)
		__field(u64, available_ns)
	),

	TP_fast_assign(
		__entry->i_ino = mapping->host->i_ino;
		if (mapping->host->i_sb)
			__entry->s_dev = mapping->host->i_sb->s_dev;
		else
			__entry->s_dev = mapping->host->i_rdev;
		__entry->start_index = start_index;
		__entry->nr_pages = nr_pages;
		__entry->lookahead_pages = lookahead_pages;
		__entry->cpuset_id = cpuset_id;
		__entry->cpuset_root = cpuset_root;
		__entry->cpuset_state = cpuset_state;
		__entry->available_pages = available_pages;
		__entry->available_ns = available_ns;
	),

	TP_printk("dev=%u:%u ino=0x%lx candidate_start_index=%lu "
		  "candidate_pages=%lu lookahead_pages=%lu cpuset_id=%llu "
		  "cpuset_root=%d cpuset_state=%u available_pages=%ld "
		  "available_ns=%llu",
		  MAJOR(__entry->s_dev), MINOR(__entry->s_dev), __entry->i_ino,
		  (unsigned long)__entry->start_index, __entry->nr_pages,
		  __entry->lookahead_pages, __entry->cpuset_id,
		  __entry->cpuset_root, __entry->cpuset_state,
		  __entry->available_pages, __entry->available_ns)
);

TRACE_EVENT(filemap_set_wb_err,
		TP_PROTO(struct address_space *mapping, errseq_t eseq),

		TP_ARGS(mapping, eseq),

		TP_STRUCT__entry(
			__field(unsigned long, i_ino)
			__field(dev_t, s_dev)
			__field(errseq_t, errseq)
		),

		TP_fast_assign(
			__entry->i_ino = mapping->host->i_ino;
			__entry->errseq = eseq;
			if (mapping->host->i_sb)
				__entry->s_dev = mapping->host->i_sb->s_dev;
			else
				__entry->s_dev = mapping->host->i_rdev;
		),

		TP_printk("dev=%d:%d ino=0x%lx errseq=0x%x",
			MAJOR(__entry->s_dev), MINOR(__entry->s_dev),
			__entry->i_ino, __entry->errseq)
);

TRACE_EVENT(file_check_and_advance_wb_err,
		TP_PROTO(struct file *file, errseq_t old),

		TP_ARGS(file, old),

		TP_STRUCT__entry(
			__field(struct file *, file);
			__field(unsigned long, i_ino)
			__field(dev_t, s_dev)
			__field(errseq_t, old)
			__field(errseq_t, new)
		),

		TP_fast_assign(
			__entry->file = file;
			__entry->i_ino = file->f_mapping->host->i_ino;
			if (file->f_mapping->host->i_sb)
				__entry->s_dev =
					file->f_mapping->host->i_sb->s_dev;
			else
				__entry->s_dev =
					file->f_mapping->host->i_rdev;
			__entry->old = old;
			__entry->new = file->f_wb_err;
		),

		TP_printk("file=%p dev=%d:%d ino=0x%lx old=0x%x new=0x%x",
			__entry->file, MAJOR(__entry->s_dev),
			MINOR(__entry->s_dev), __entry->i_ino, __entry->old,
			__entry->new)
);
#endif /* _TRACE_FILEMAP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
