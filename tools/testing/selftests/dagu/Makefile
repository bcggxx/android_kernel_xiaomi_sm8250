# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := test_miui_tracking_parser.py test_seq_file_ra.py \
	test_dagu_reclaim.py test_graphics_priority.py test_zsmalloc_pool.py test_limited_readahead.py
TEST_FILES := c_helpers.py README
TEST_GEN_FILES := kernel-sources.tar

include ../lib.mk

# Keep installed tests independent of the original source checkout.
KERNEL_SRC := $(abspath ../../../..)
SOURCES := drivers/block/zram/zram_drv.c fs/f2fs/file.c \
	mm/readahead_limit.c mm/kshrink_slabd.c mm/mi_rmap_efficiency.c mm/vmscan.c \
	drivers/staging/binder_prio/binder_prio.c mm/zsmalloc.c

$(OUTPUT)/kernel-sources.tar: $(addprefix $(KERNEL_SRC)/,$(SOURCES)) Makefile
	mkdir -p $(OUTPUT)
	tar -cf $@ -C $(KERNEL_SRC) $(SOURCES)
