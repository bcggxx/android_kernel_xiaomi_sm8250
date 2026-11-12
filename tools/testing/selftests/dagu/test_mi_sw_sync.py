#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the mi_sw_sync create, signal, and close paths on a target."""

import errno
import fcntl
import os
import select
import struct
import sys


DEVICE = "/dev/mi_sw_sync"
CREATE_FENCE = 0xC0285700
INCREMENT = 0x40045701
CREATE_DATA = struct.Struct("<I32si")


def create_fence(timeline_fd, value):
    data = bytearray(CREATE_DATA.pack(value, b"mi-sw-sync-test", 0))
    fcntl.ioctl(timeline_fd, CREATE_FENCE, data, True)
    fence_fd = CREATE_DATA.unpack(data)[2]
    if fence_fd < 0:
        raise AssertionError("kernel returned a negative fence fd")
    return fence_fd


def fence_signaled(fence_fd, timeout_ms):
    poller = select.poll()
    poller.register(fence_fd, select.POLLIN)
    return bool(poller.poll(timeout_ms))


def test_signal(timeline_fd):
    fence_fd = create_fence(timeline_fd, 1)
    try:
        if fence_signaled(fence_fd, 50):
            raise AssertionError("fence signaled before timeline increment")
        fcntl.ioctl(timeline_fd, INCREMENT, struct.pack("<I", 1))
        if not fence_signaled(fence_fd, 1000):
            raise AssertionError("fence did not signal after timeline increment")
    finally:
        os.close(fence_fd)


def test_close_wakes_fence():
    timeline_fd = os.open(DEVICE, os.O_RDWR | os.O_CLOEXEC)
    fence_fd = create_fence(timeline_fd, 1)
    try:
        os.close(timeline_fd)
        timeline_fd = -1
        if not fence_signaled(fence_fd, 1000):
            raise AssertionError("timeline close did not wake pending fence")
    finally:
        if timeline_fd >= 0:
            os.close(timeline_fd)
        os.close(fence_fd)


def main():
    if not os.path.exists(DEVICE):
        print(f"[SKIP] {DEVICE} is unavailable")
        return 4

    timeline_fd = os.open(DEVICE, os.O_RDWR | os.O_CLOEXEC)
    try:
        try:
            fcntl.ioctl(timeline_fd, 0x12345678)
        except OSError as exc:
            if exc.errno != errno.ENOTTY:
                raise
        else:
            raise AssertionError("unknown ioctl unexpectedly succeeded")

        test_signal(timeline_fd)
    finally:
        os.close(timeline_fd)

    test_close_wakes_fence()
    print("mi_sw_sync create/signal/close: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
