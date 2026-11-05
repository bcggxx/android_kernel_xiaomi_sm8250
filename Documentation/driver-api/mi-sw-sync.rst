.. SPDX-License-Identifier: GPL-2.0

=============
MI SW Sync
=============

The optional ``CONFIG_MI_SW_SYNC`` device exposes Xiaomi's software-sync
interface as ``/dev/mi_sw_sync``.  The interface is disabled by default.

Each successful open creates an independent software timeline.  The timeline
is owned by the open file and remains alive while fences created from it are
still referenced.

The device supports two ioctls from ``include/uapi/linux/mi_sw_sync.h``:

``SW_SYNC_IOC_CREATE_FENCE``
  Takes ``struct sw_sync_create_fence_data``.  The kernel creates a fence with
  the requested sequence number and returns a ``sync_file`` fd in ``fence``.

``SW_SYNC_IOC_INC``
  Takes a ``__u32`` increment.  The timeline value is advanced and every
  pending fence whose sequence number is no longer later than the timeline
  value is signaled.

Closing the timeline file while fences are pending gives those fences
``-ENOENT`` and signals them before dropping the file's timeline reference.
Closing a fence fd only drops that fence's reference.  A fence fd is not
duplicated by the device; standard file-descriptor rules apply.

The device follows the Xiaomi software-sync ABI and intentionally does not
depend on debugfs.  The implementation does not add debugfs nodes or expose
timeline state.  Its DAC mode is ``0666`` to match the device-tree setup used
by Xiaomi display userspace; SELinux policy still controls which domains may
open it.
