SM8250 MIUI feature delivery
===========================

``build_kernel.sh <device> [ksu] [miui|aosp] [droidspaces]`` merges
``arch/arm64/configs/vendor/sm8250-miui-features.config`` for every supported
device in MIUI/HyperOS builds only, after the ROM and optional DroidSpaces
tweaks. AOSP builds retain their existing configuration path and do not merge
or validate this MIUI fragment.
The single-device and all-device workflows call this same entry point.
After ``olddefconfig``, the script verifies every setting in the fragment
and stops if a dependency prevented it from taking effect.

The fragment compiles the shared MIUI features into MIUI release kernels. It retains
their existing runtime controls: launch collection, memory-engine sessions,
optional reclaim policies and pools require their explicit controls; software
sync requires a userspace client. The existing Binder policy remains active
when built, with its extra main-thread rules disabled by default. The Moon RT
direct-reclaim throttle bypass also takes effect when built; it has no runtime
enable switch. These retain the previous dagu build's semantics and now ship
through the MIUI configuration for the other supported devices. Software
sync registers its misc device with the existing mode 0666; ROM device rules
and SELinux still determine deployed access. Union Power creates its interfaces
and cleanup workqueue at init even while frame monitoring is disabled. No ROM
integration is installed by the kernel packaging step. READAHEAD_LIMIT and
diagnostic fault injection remain disabled by this MIUI fragment.

For MIUI builds, direct ``make <device>_defconfig`` users can apply the same fragment without
running the full build script, which also performs remote setup and packaging::

    make O=out ARCH=arm64 <device>_defconfig
    scripts/kconfig/merge_config.sh -m -O out out/.config \
        arch/arm64/configs/vendor/sm8250-miui-features.config
    make O=out ARCH=arm64 olddefconfig

These features serve the supported SM8250-family devices; dagu is the
representative test device. Device configuration, test-suite and historical
artifact names may still contain its codename. Shared launch functionality
uses ``launch_profile`` and ``CONFIG_LAUNCH_PROFILE``; see
``tools/launch_profile/README.md`` for the coordinated userspace migration.
