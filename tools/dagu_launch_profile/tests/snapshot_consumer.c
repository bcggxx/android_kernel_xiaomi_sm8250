/* SPDX-License-Identifier: Apache-2.0 */
#define main helper_main
#include "../dagu_launch_profile_helper.c"
#undef main
#include <assert.h>

static int snapshot(uint64_t length)
{
    char path[] = "/tmp/dlp-snapshot-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    uint32_t h[8] = {htole32(0x31504c44), htole32(1), htole32(56),
        htole32(1), htole32(1), 0, htole32(12), 0};
    struct { uint32_t index, reserved; uint64_t offset, length; } r =
        {0, 0, 0, htole64(length)};
    assert(write(fd, h, sizeof(h)) == sizeof(h));
    assert(write(fd, &r, sizeof(r)) == sizeof(r));
    assert(lseek(fd, 0, SEEK_SET) == 0);
    return fd;
}
int main(void)
{
    int files[128];
    memset(files, -1, sizeof(files));
    char path[] = "/tmp/dlp-source-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, 5000) == 0);
    files[0] = open(path, O_RDONLY | O_CLOEXEC);
    unlink(path);
    close(fd);
    assert(files[0] >= 0);
    unsigned int skipped = 0;
    // A short successful read spanning pages 0 and 1 has an 8192-byte page range,
    // including a rounded tail beyond EOF; consumer must accept that exact wire.
    int wire = snapshot(8192);
    assert(consume_snapshot(wire, files, &skipped) == 0 && skipped == 0);
    close(wire);
    // Do not let budget subtraction wrap for an attacker-controlled LE u64.
    wire = snapshot(UINT64_MAX & ~4095ULL);
    assert(consume_snapshot(wire, files, &skipped) == -EINVAL);
    close(wire);
    close(files[0]);
    puts("production snapshot consumer: cross-page EOF and overflow bounds PASS");
    return 0;
}
