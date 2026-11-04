# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := test_launch_finish.py test_kernel_version_reader.py test_miui_tracking_parser.py test_seq_file_ra.py \
	test_dagu_reclaim.py test_global_slab.py test_mi_async_reclaim.py test_mi_mtmr.py test_mi_mtmr_ra.py test_mi_mtmr_control.py test_graphics_priority.py test_zsmalloc_pool.py test_limited_readahead.py test_mi_fault_around.py test_mi_reclaim_pool.py test_mi_reclaim_pool_raw.py
TEST_FILES := c_helpers.py README
TEST_GEN_PROGS := mem_engine/session
TEST_GEN_FILES := kernel-sources.tar

include ../lib.mk

# Keep installed tests independent of the original source checkout.
KERNEL_SRC := $(abspath ../../../..)
SOURCES := mm/dagu_launch_profile.c tools/kernel-version/read.sh drivers/block/zram/zram_drv.c fs/f2fs/file.c \
	mm/mi_reclaim_pool.c mm/allocator_reserve.c mm/mi_fault_around.c mm/mi_async_reclaim.c mm/mi_mtmr.c mm/readahead_limit.c mm/kshrink_slabd.c mm/mi_rmap_efficiency.c mm/vmscan.c \
	drivers/staging/binder_prio/binder_prio.c mm/zsmalloc.c

$(OUTPUT)/kernel-sources.tar: $(addprefix $(KERNEL_SRC)/,$(SOURCES)) Makefile
	mkdir -p $(OUTPUT)
	tar -cf $@ -C $(KERNEL_SRC) $(SOURCES)
