Target kernel release metadata
==============================

CONFIG_CHIRUNO_KERNEL_VERSION is an optional tristate, dependent on PROC_FS,
with default n. dagu_defconfig explicitly leaves it disabled. When enabled,
/proc/chiruno_kernel_version is read-only (0444) and contains this build's
UTS_RELEASE followed by exactly one LF. It must match uname -r on that target.
It is not a source hash, module ABI guarantee, ROM compatibility code or a
performance feature. No runtime string override or write interface exists.

The module name is chiruno_kernel_version. A module build can be unloaded;
proc_remove removes only the entry registered by this module and uses the
4.19 procfs rundown mechanism for active callbacks/openers. single_release
owns each open's seq state. A registration failure returns -ENOMEM and does
not remove any existing same-name entry. Built-in enablement lasts until
shutdown; disabling it requires booting a build configured without it.

Maintenance consumer
--------------------

Copy tools/kernel-version/read.sh to the Android host, then run it there::

  sh /data/local/tmp/read-kernel-version.sh

For example, from the Linux container, with an already connected adb target::

  timeout -k 2s 15s adb push tools/kernel-version/read.sh /data/local/tmp/read-kernel-version.sh
  timeout -k 2s 15s adb shell sh /data/local/tmp/read-kernel-version.sh

These are manual commands, not an automatic retry script. If offline, inspect
``adb devices``, reconnect the selected target and retry at most twice; for a
persistent hang restart the server and restore the original connection.
Check whether a push already completed before repeating it. Do not run the
reader in the container: its uname describes the wrong kernel.

The reader requires only the Android shell, cat and uname. It verifies a
nonempty exact single line including its final LF against the same host's
uname -r and prints the interface path and release. Missing, unreadable,
malformed or mismatched data exits nonzero. SELinux denial remains a failure;
the tool does not change SELinux or enable other policy. It is an interactive
maintenance consumer, not evidence that a private ROM service uses this node.

Source and verification boundaries
----------------------------------

This implementation was independently written using this tree's procfs and
seq_file APIs. Behavioral reference: NikoRur_QwQRuaa's public
NikoRurQwQRuaaMeow/chirunoneko_gki_android13-5.15_kernel_module_opensource,
commit 96dd2f7849a880cb693f371bc0fc8595645dda2f, Chiruno/kernel_version.
That source exposes the literal placeholder ``version-code`` plus LF; this
implementation deliberately identifies the target build instead. Its C file
states MODULE_LICENSE("GPL") but lacks SPDX/full file licensing, while the
repository LICENSE is Apache-2.0. No source text from that file was copied;
this note does not resolve that donor licensing ambiguity.

The host selftest test_kernel_version_reader.py exercises this production
reader with controlled command responses. It cannot establish procfs VFS
behavior or identity on an actual dagu kernel. A related object compile checks
the 4.19 file_operations API, not loading or runtime behavior.

Target acceptance remains required: with a matching module build, load it,
run the real reader, perform repeated/partial reads and close, retain an open
fd while unloading, check that new opens fail, and inspect logs for warnings
or Oops. A disabled build must have no node. Existing open fds have no promise
of continued readability after removal. No device enablement, load/unload,
full image build, regression measurement or performance claim is implied by
these host checks. Unknown private ROM compatibility remains unverified.
