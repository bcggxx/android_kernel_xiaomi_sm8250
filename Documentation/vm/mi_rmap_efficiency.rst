High-mapcount file-page protection
==================================

CONFIG_MI_RMAP_EFFICIENCY enables a reclaim policy that retains mapped file
pages to reduce repeated unmapping and refaults. The Kconfig option defaults
to disabled. When compiled in, the policy becomes active only after its
sysfs interface has been initialized successfully and enabled is set to 1.
The built-in runtime default remains false.

Eligibility and budgets
-----------------------

The policy considers ordinary mapped file pages. Anonymous, swap-backed and
compound pages are excluded. It applies only to non-forced global order-0
reclaim. Memcg limit reclaim and DMA/DMA32 allocation reclaim bypass it.
Available memory must exceed 512 MiB at the start of the reclaim batch.

Pages whose mapcount reaches the configured threshold are activated. The
default threshold is 20. At intermediate reclaim priorities a lower
priority-dependent threshold can retain pages without activation. Both
outcomes consume the same protection budget. On the active file list, either
outcome retains the page on the active list and counts it as rotated. Active
and inactive scans share the same scan_control protection counter.

At priorities 7 through 9, an eligible active page at the lower threshold
can bypass page_referenced after protection declines it, then takes the
ordinary ClearPageActive/SetPageWorkingset path. With stable controls this
occurs when the protection budget is exhausted: the bypass deactivates the
page and consumes no protection budget. It does not retain a page after the
protection cap. Disabled, ineligible and pressure-exit decisions use normal
reference checking. Available memory is sampled once per active or inactive
batch; a pressure drop inside that batch is observed by the next batch.

Protection is capped at 16 MiB per scan_control context. Under increased
pressure, a scan-relative budget also applies. At priority DEF_PRIORITY / 2
or below, the policy stops protecting pages. Retaining file pages can defer
reclamation and increase pressure on other pages; these limits bound the
policy rather than guarantee a particular latency or throughput improvement.

Runtime control
---------------

The controls reside in /sys/kernel/mi_rmap_efficiency/:

* enabled: boolean runtime switch.
* mapcount_thres: activation threshold, from 2 through INT_MAX, default 20.

Changing a control affects subsequent policy decisions. Disabling does not
immediately reclaim pages retained by earlier decisions.

See Documentation/ABI/testing/sysfs-kernel-mi-rmap-efficiency for the ABI.

Source and validation boundaries
-------------------------------

This is a bounded page-based adaptation of MiCode/Xiaomi_Kernel_OpenSource
spring-v-oss commit e29c0fd34c6e8f91d30046a85d1afac3fe032a7e,
drivers/staging/mi_rmap_efficiency/mi_rmap_efficiency.c (author maminghui5;
original MODULE_LICENSE("GPL")). The original does not narrow its license
version. Existing target code retains its own SPDX declaration. The target
keeps its threshold 20, ordinary-file eligibility, pressure exits and strict
shared budgets rather than copying folio layouts, vendor data or counters.
The distinct 96dd2f screenshot-source identity remains unconfirmed.

Host policy tests execute policy C with userspace stubs. They do not prove
LRU isolation accounting, page lifetime under concurrency, device reclaim
progress, boot behavior or performance. Those require target validation.
