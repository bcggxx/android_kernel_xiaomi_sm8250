High-mapcount file-page protection
==================================

CONFIG_MI_RMAP_EFFICIENCY enables a reclaim policy that retains mapped file
pages to reduce repeated unmapping and refaults. The Kconfig option defaults
to disabled. When compiled in, the policy becomes active only after its
sysfs interface has been initialized successfully.

Eligibility and budgets
-----------------------

The policy considers ordinary mapped file pages. Anonymous, swap-backed and
compound pages are excluded. It applies only to non-forced global order-0
reclaim. Memcg limit reclaim and DMA/DMA32 allocation reclaim bypass it.
Available memory must exceed 512 MiB at the start of the reclaim batch.

Pages whose mapcount reaches the configured threshold are activated. The
default threshold is 20. At intermediate reclaim priorities a lower
priority-dependent threshold can retain pages without activation. Both
outcomes consume the same protection budget.

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
