# ApartTUSITU’s Xiaomi SM8250 Kernel

This kernel is under development; use it with care. It is based on
[LineageOS/android_kernel_qcom_sm8250](https://github.com/LineageOS/android_kernel_qcom_sm8250),
with contributions from [Strawing](https://github.com/liyafe1997/kernel_xiaomi_sm8250_mod),
[UtsavBalar1231](https://github.com/UtsavBalar1231/kernel_xiaomi_sm8250),
[LineageOS/android_kernel_xiaomi_sm8250](https://github.com/LineageOS/android_kernel_xiaomi_sm8250),
and other open-source developers.

## Features / 特性

- Supports [ReSukiSU](https://github.com/ReSukiSU/ReSukiSU) and
  [SuSFS](https://gitlab.com/simonpunk/susfs4ksu); install the ReSukiSU manager
  separately. NoKernelSU builds support Magisk and APatch and their forks.
- F2FS realtime discard, EROFS, and zRAM with LZO, LZ4, LZ4HC, ZSTD and
  writeback support.
- Backported BPF and `clone3` support for Android 16/17, plus
  [LE9EC](https://github.com/hakavlad/le9-patch) memory optimizations.
- Binder 5.10 backport with MIUI millet or AOSP [Re:Kernel](https://github.com/Sakion-Team/Re-Kernel).
  See [Binder priority policy](Documentation/driver-api/binder-priority.rst)
  for the documented policy.
- Fix for [battery percentage stuck at 1%](https://github.com/liyafe1997/Xiaomi-fix-battery-one-percent),
  support for higher-capacity replacement batteries, and
  [Baseband-guard](https://github.com/vc-teahouse/Baseband-guard).

Release packages are built from `android17-aptusitu` and are intended for stock
MIUI/HyperOS and AOSP-based Android 11–17 ROMs. Compatibility can vary by device
and ROM; report issues through GitHub Issues or Pull Requests.

## Supported devices / 支持的设备

| Codename | Device |
| --- | --- |
| psyche | Xiaomi 12X |
| thyme | Xiaomi 10S |
| umi | Xiaomi 10 |
| munch | Redmi K40S / POCO F4 |
| lmi | Redmi K30 Pro / POCO F2 Pro |
| cmi | Xiaomi 10 Pro |
| cas | Xiaomi 10 Ultra |
| apollo | Xiaomi 10T / Redmi K30S Ultra |
| alioth | Xiaomi 11X / POCO F3 / Redmi K40 |
| elish | Xiaomi Pad 5 Pro |
| enuma | Xiaomi Pad 5 Pro 5G |
| dagu | Xiaomi Pad 5 Pro 12.4 |
| pipa | Xiaomi Pad 6 |

## Community / 社区

ApartTUSITU’s QQ group: [700675046](https://qm.qq.com/q/Md7nXA3Toa). Coolapk
discussion: [post 69700122](https://www.coolapk.com/feed/69700122).

## Build / 构建

For one device, run:

```sh
./build_kernel.sh <device> [ksu] [miui|aosp] [droidspaces]
```

Omitting the ROM selects both AOSP and MIUI builds for that device. Use the
**Build Kernel** GitHub Actions workflow for a single device or **Build All
Devices Kernel (Matrix Parallel + Release)** for the device matrix. For Dagu
toolchain selection, outputs, script side effects, and CI coverage, see
[Dagu build notes](Documentation/kbuild/dagu-build.md). The script and workflow
files define the current build behavior.

The shared [MIUI/HyperOS feature configuration](Documentation/kbuild/sm8250-features.rst)
applies to all supported devices on the `miui` build path. AOSP builds keep
their separate configuration path. Both ROM variants use the neutral
[release identity](Documentation/kbuild/release-identity.rst).
