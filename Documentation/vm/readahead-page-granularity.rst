.. SPDX-License-Identifier: GPL-2.0

Buffered readahead page granularity on dagu
==========================================

Scope and source
----------------

This note records the existing file-page allocation behavior of the dagu
ARM64 4.19 tree, audited at commit
42ff4ea88d67e88208038424487bf2a7400a0d67. It covers the buffered file
readahead path for ext4, F2FS and EROFS. It changes no allocation, window,
filesystem, configuration or runtime default.

The comparison is with the standard Android common android17-6.18 hook
``android_vh_page_cache_ra_order_bypass``, at the fixed revision
01c7e6b174af760d2ae7d975ab8d6f2597c2d1ec:

https://android.googlesource.com/kernel/common/+/01c7e6b174af760d2ae7d975ab8d6f2597c2d1ec/

In that revision, include/trace/hooks/mm.h declares the hook with
``readahead_control``, ``file_ra_state``, new_order, gfp and bypass arguments.
In mm/readahead.c, page_cache_ra_order() initializes bypass to false and
branches to fallback when a probe sets it true. The fallback calls
do_page_cache_ra() for the remaining window; page_cache_ra_unbounded()
allocates at mapping_min_folio_order(mapping). Thus bypass means using the
mapping's minimum allocation granularity instead of adaptive large-folio
readahead. It does not universally mean physical order zero. Setting
file_ra_state.order to zero changes history, not the filesystem's minimum.

The madrid dynamic_readahead module audit selects ext4, F2FS and EROFS for
this bypass and clears that history field. This note uses the public hook
and consumer as the comparison contract; it does not claim that this common
revision is the exact madrid vendor build, or that the incomplete public
Chiruno directory is the module's source. No vendor implementation is copied.
Vendor-specific mapping minimum orders and source identity remain unknown.

Allocation and submission
-------------------------

In this tree, __do_page_cache_readahead() in mm/readahead.c checks EOF and
existing cache entries, then calls __page_cache_alloc() once per missing
page index. It places those pages in page_pool before read_pages() invokes
the filesystem's readpages callback. If there is no readpages callback,
read_pages() inserts the same pages and invokes readpage individually.

Both implementations of __page_cache_alloc() specify order zero:

* Without CONFIG_NUMA, include/linux/pagemap.h uses alloc_pages(gfp, 0).
* With CONFIG_NUMA, mm/filemap.c uses __alloc_pages_node(n, gfp, 0) for
  cpuset memory spreading and alloc_pages(gfp, 0) otherwise. Node selection
  and the cpuset retry do not change the requested order.

There is no adaptive higher-order allocation in this selected path. Its
existing base-page granularity provides the target counterpart of the
standard minimum-granularity fallback. This is a scoped equivalence of
file-cache allocation, not a claim of identical algorithms or all-file IO.
It does not depend on task identity, low memory, or the experimental
readahead window policy being enabled.

Filesystem consumers
--------------------

ext4
  ext4_set_aops() in fs/ext4/inode.c selects ext4_aops,
  ext4_da_aops or ext4_journalled_aops for buffered files. All three use
  ext4_readpages()/ext4_readpage(). ext4_readpages() skips inline data or
  calls ext4_mpage_readpages() in fs/ext4/readpage.c. That function removes
  each supplied page from the list and inserts that same page through
  add_to_page_cache_lru(), then maps blocks and assembles IO. Buffer-head
  fallback fills the supplied page; it does not replace it with a large
  file-cache allocation. Inline readpage also fills the supplied page.

F2FS
  f2fs_dblock_aops in fs/f2fs/data.c uses f2fs_read_data_pages() and
  f2fs_read_data_page(). The former skips inline data or an unavailable
  compression backend, otherwise calling f2fs_mpage_readpages(). The latter
  fills inline data or passes the supplied single page to that helper.
  f2fs_mpage_readpages() removes and inserts each supplied list page through
  add_to_page_cache_lru() before its ordinary or compressed-file handling.
  Compression collects these file pages in compress_ctx.rpages and uses
  f2fs_read_multi_pages(); cluster size does not turn them into compound
  file-cache pages. Additional decompression pages and compressed-block
  cache pages in fs/f2fs/compress.c serve separate buffers/mappings.

EROFS
  fs/erofs/inode.c initially installs erofs_raw_access_aops;
  compressed-file setup in fs/erofs/zmap.c installs z_erofs_aops.
  erofs_raw_access_readpages() in fs/erofs/data.c inserts the supplied
  pages through add_to_page_cache_lru() before erofs_read_raw_page().
  z_erofs_readpages() in fs/erofs/zdata.c likewise inserts the supplied
  file pages, then passes them to z_erofs_do_read_page() and the
  decompression queue. The separate managed compressed cache (mc),
  pclusters and temporary pagepool are not larger replacement allocations
  for the file pages supplied by generic readahead. readpage callbacks
  operate on the supplied single file page as well.

BIO merging, encryption buffers and compression clusters can change IO or
working-buffer sizes without changing these file-cache pages' order. DAX
(ext4_dax_aops), direct IO, anonymous THP and other non-buffered paths are
outside this comparison. This note introduces no blanket restriction on
those paths or on filesystem metadata/compressed caches.

Failure, controls and validation boundary
-----------------------------------------

readahead_gfp_mask() adds __GFP_NORETRY and __GFP_NOWARN to the mapping mask.
If __page_cache_alloc() fails, __do_page_cache_readahead() stops collecting
pages and submits any collected batch. It does not retry with a higher
order. read_pages() releases leftovers after readpages returns. Cache
insertion races and filesystem filtering may discard candidates. Readahead
IO errors are handled by subsequent demand readpage and normal error paths;
order zero does not guarantee allocation or IO success under pressure.

No new switch is needed: the selected path already has minimum granularity
whether a hypothetical large-folio bypass is on or off. There is no new
worker, pinned state or shutdown operation. Existing demand reading and
readahead-limit controls retain their behavior. See readahead-limit.rst for
that independent window policy; its earlier acceptance is not reopened.

Validation here is source-path inspection only, including both NUMA
branches and all a_ops variants above. No kernel build, CI, boot, allocation
failure injection, actual filesystem read or performance measurement was
performed for this documentation change. Source equivalence is delivered;
real-device functional acceptance, pressure recovery and performance remain
unconfirmed. Future runtime evidence must correlate actual buffered reads
with file-page allocation order and demand results, including an allocation
failure and recovery, rather than inferring page size from BIO or cluster
size. This note does not close fault-around, identity or userspace-control
work for the rest of dynamic_readahead.
