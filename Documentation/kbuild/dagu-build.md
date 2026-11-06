# Dagu kernel build

These notes cover the `dagu` target (Xiaomi Pad 5 Pro 12.4) in this SM8250
source tree. `build_kernel.sh` and the workflows remain the source of truth for
the current implementation.

## Entry point

These commands document the script interface; they do not record a completed
local build. From the source root, build Dagu for both ROM variants with:

```sh
bash build_kernel.sh dagu
```

Select one ROM with `miui` or `aosp`; add `ksu` for ReSukiSU/SuSFS or
`droidspaces` for its optional patches. For example, a MIUI build with KSU and
DroidSpaces uses:

```sh
bash build_kernel.sh dagu ksu miui droidspaces
```

Omitting the ROM builds AOSP and MIUI for Dagu. It does not build every device;
use the **Build All Devices Kernel (Matrix Parallel + Release)** workflow for
the device matrix. The **Build Kernel** workflow accepts one device and lets
you choose the ROM, KernelSU, and DroidSpaces options.

MIUI/HyperOS builds merge the common
`arch/arm64/configs/vendor/sm8250-miui-features.config` for all supported
devices and verify its resolved settings. AOSP builds keep their own path.
See [shared MIUI feature delivery](sm8250-features.rst) for configuration
details and [release identity](release-identity.rst) for the neutral version
suffix and build user/host. Dagu is the representative test device.

Complete Dagu image delivery in the Kernels workspace follows its cloud CI
policy. Local LLVM 19 object compilation is not a substitute for that gate.

## Host and toolchain

Dagu is an ARM64 kernel target (`ARCH=arm64`, `SUBARCH=arm64`). The build host
can be x86_64: Clang and other host programs must run natively on that host,
while the ARM cross tools produce the target objects. The script checks for
Clang/LLVM tools, `aarch64-linux-gnu-as`, `arm-linux-gnueabi-as`, and `ccache`
before it starts remote setup.

The GitHub build workflows download the fixed
[ZyCromerZ Clang 16.0.6-20260807 release](https://github.com/ZyCromerZ/Clang/releases/tag/16.0.6-20260807-release)
into `$HOME/zyc-clang`. Locally, `build_kernel.sh` uses this precedence:

1. `TOOLCHAIN_BIN`, when set to a toolchain `bin` directory.
2. `$HOME/zyc-clang/bin`, when it contains an executable `clang`.
3. `/usr/lib/llvm-19/bin` otherwise.

The current WSL x86_64 setup uses native LLVM 19 through the third path. This
host/toolchain combination has passed local object compilation; that evidence
does not establish that the script produced a complete kernel image. Current
workspace validation state is recorded in `sm8250/reports/dagu-current.md`
when that workspace file is available.

## Outputs and source-tree effects

The script writes per-ROM build trees named `out_aosp` and `out_miui`, and
removes the selected tree before rebuilding it. It stores compiler cache data
under `$HOME/.cache/ccache_mikernel`. Successful builds package the kernel
image, DTB, and any available DTBO into an `APTKernel_*.zip` at the source
root.

The full entry point also performs setup that changes the checkout:

- Baseband-guard setup downloads and runs its remote setup script and edits
  `security/Kconfig`.
- With `ksu`, the script fetches the ReSukiSU setup script at a full commit
  SHA. The default is `34210a4dec297cb48ce4bfa1c5ba50a2f7d22641`; an override
  must also be a full SHA.
- With `droidspaces`, it downloads patches from the upstream `main` branch,
  applies those that match, and merges a temporary configuration fragment.
- It deletes and reclones `anykernel` from the `kona` branch, then clears
  `anykernel/kernels/*` while packaging each ROM variant.
- A MIUI build edits Qualcomm panel DTS files in place and restores its backup
  after compilation. If the script exits before restoration, inspect and
  recover those files from the `.dts.bak.miui` backup before continuing.

The Baseband-guard setup URL, DroidSpaces `main`, and AnyKernel3 `kona` branch
are not pinned by this script. Preserve local source changes and inspect the
working tree before running a full build; use a disposable checkout when those
remote setup effects are not acceptable.

## CI and evidence limits

The single-device **Build Kernel** workflow runs the full build script on
Ubuntu 24.04 with the pinned ZyCromerZ Clang 16 release and uploads the
flashable ZIP. Shared MIUI feature code is compiled through the common release
configuration rather than a separate Dagu-only object step. The matrix workflow
builds the listed devices and can publish their artifacts.

`sm8250-host-check.yml` runs the Dagu user-space selftest suite with ASan/UBSan and a
profile-publication test. Passing those host checks is not an ARM64 kernel
build, boot, or device test. Local LLVM 19 object compilation also does not
replace the full-image CI evidence required by the Kernels workspace policy;
that policy lives in `docs/agents/validation.md` when the parent workspace is
available. A standalone source checkout should judge delivery from its own
build workflow and artifact evidence.
