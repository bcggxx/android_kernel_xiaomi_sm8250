/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/dagu_launch_profile.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __ANDROID__
#include <cutils/sockets.h>
#endif

struct packet {
    uint32_t version, operation;
    uint64_t cookie;
    uint32_t uid, lease_ms;
    uint32_t tid, binder_handle;
};
_Static_assert(sizeof(struct packet) == 32, "wire size");

static struct dlp_request request(uint64_t cookie, uint64_t epoch, uint32_t op)
{
    struct dlp_request r = { .version = 1, .size = sizeof(r), .cookie = cookie,
        .epoch = epoch, .op = op };
    return r;
}

/* Consume the actual immutable kernel snapshot and every corresponding read-only
 * file fd. Ticket 07 deliberately does not publish a disk training profile. */
static int consume_snapshot(int snapshot, const int files[128], unsigned int *skipped)
{
    uint32_t header[8];
    struct { uint32_t index, reserved; uint64_t offset, length; } record;
    if (read(snapshot, header, sizeof(header)) != sizeof(header)) return -EINVAL;
    uint32_t count = le32toh(header[3]), ranges = le32toh(header[4]);
    uint32_t shift = le32toh(header[6]);
    if (le32toh(header[0]) != 0x31504c44 || le32toh(header[1]) != 1
            || count > 128 || ranges > 4096 || (le32toh(header[5]) & ~1U)
            || shift < 12 || shift > 16
            || le32toh(header[2]) != sizeof(header) + ranges * sizeof(record)) return -EINVAL;
    uint64_t bytes = 0, last_end = 0;
    uint32_t last_index = 0;
    for (uint32_t i = 0; i < ranges; i++) {
        struct stat st;
        if (read(snapshot, &record, sizeof(record)) != sizeof(record)) return -EINVAL;
        uint32_t index = le32toh(record.index);
        uint64_t offset = le64toh(record.offset), length = le64toh(record.length);
        uint64_t mask = (1ULL << shift) - 1;
        if (record.reserved || index >= count || !length || offset > UINT64_MAX - length
                || ((offset | length) & mask)
                || length > 32ULL * 1024 * 1024
                || bytes > 32ULL * 1024 * 1024 - length
                || (i && (index < last_index || (index == last_index && offset <= last_end))))
            return -EINVAL;
        if (files[index] < 0) (*skipped)++;
        else {
            if (fstat(files[index], &st) || !S_ISREG(st.st_mode) || st.st_size < 0
                    || offset >= (uint64_t)st.st_size
                    || (uint64_t)st.st_size > UINT64_MAX - mask) return -EINVAL;
            uint64_t rounded_eof = ((uint64_t)st.st_size + mask) & ~mask;
            if (length > rounded_eof - offset) return -EINVAL;
        }
        bytes += length;
        last_index = index;
        last_end = offset + length;
    }
    char extra;
    if (read(snapshot, &extra, 1) != 0) return -EINVAL;
    fprintf(stderr, "DLP1 consumed files=%u ranges=%u bytes=%llu flags=%u dropped=%u skipped=%u\n",
            count, ranges, (unsigned long long)bytes, le32toh(header[5]),
            le32toh(header[7]), *skipped);
    return 0;
}

static int consume(int controller, uint64_t cookie, uint64_t epoch)
{
    struct dlp_request r = request(cookie, epoch, DLP_SNAPSHOT);
    int snapshot = ioctl(controller, DLP_IOCTL, &r);
    int files[128], result = -EINVAL;
    uint32_t header[8];
    unsigned int skipped = 0;
    memset(files, -1, sizeof(files));
    if (snapshot < 0) return -errno;
    if (pread(snapshot, header, sizeof(header), 0) != sizeof(header)) goto out;
    uint32_t count = le32toh(header[3]);
    if (count > 128) goto out;
    for (uint32_t i = 0; i < count; i++) {
        struct stat st;
        r = request(cookie, epoch, DLP_EXPORT_FILE);
        r.index = i;
        files[i] = ioctl(controller, DLP_IOCTL, &r);
        // Writable collected descriptions are intentionally never exported.
        if (files[i] < 0 && errno == EACCES) continue;
        if (files[i] < 0) { result = -errno; goto out; }
        int flags = fcntl(files[i], F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY
                || fstat(files[i], &st) || !S_ISREG(st.st_mode)) goto out;
    }
    result = consume_snapshot(snapshot, files, &skipped);
out:
    for (unsigned i = 0; i < 128; i++) if (files[i] >= 0) close(files[i]);
    close(snapshot);
    return result;
}

static int authorized(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);
    char context[256];
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != 1000)
        return 0;
    len = sizeof(context) - 1;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERSEC, context, &len) || len >= sizeof(context))
        return 0;
    context[len] = 0;
    return !strcmp(context, "u:r:system_server:s0");
}

/* Every received descriptor is closed, including on malformed/truncated packets. */
static int receive(int fd, struct packet *p, int *pidfd)
{
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = { .iov_base = p, .iov_len = sizeof(*p) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    unsigned int count = 0;
    *pidfd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); n >= 0 && c;
         c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS
                || c->cmsg_len < CMSG_LEN(0)) continue;
        size_t bytes = c->cmsg_len - CMSG_LEN(0);
        int *fds = (int *)CMSG_DATA(c);
        for (size_t i = 0; i < bytes / sizeof(int); i++) {
            if (count++ == 0) *pidfd = fds[i];
            else close(fds[i]);
        }
    }
    if (n != sizeof(*p) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || count > 1) {
        if (*pidfd >= 0) close(*pidfd);
        *pidfd = -1;
        return -1;
    }
    return 0;
}

static void serve(int peer)
{
    int controller = -1;
    struct timeval tv = { .tv_sec = 1 };
    if (!authorized(peer)) goto out;
    if (setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))
            || setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) goto out;
    controller = open("/dev/dagu_launch_profile", O_RDWR | O_CLOEXEC);
    if (controller < 0) goto out;
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now)) goto out;
    uint64_t epoch = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
    struct dlp_request enable = request(0, epoch, 1);
    enable.flags = 1;
    if (ioctl(controller, DLP_IOCTL, &enable)) goto out;
    for (;;) {
        struct pollfd fds[1] = {{ .fd = peer, .events = POLLIN }};
        int ready = poll(fds, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (!(fds[0].revents & POLLIN)) continue;
        struct packet p;
        int received = -1;
        if (receive(peer, &p, &received)) break;
        uint32_t op = le32toh(p.operation);
        uint64_t cookie = le64toh(p.cookie);
        struct dlp_request r = request(cookie, epoch, 2);
        int status = -EINVAL;
        if (le32toh(p.version) == 1 && cookie) {
            if (op == 1 && received >= 0 && le32toh(p.lease_ms) > 0
                    && le32toh(p.lease_ms) <= 2000) {
                r.lease_ms = le32toh(p.lease_ms);
                r.binder_fd = received;
                r.binder_handle = le32toh(p.binder_handle);
                r.tid = le32toh(p.tid);
                r.uid = le32toh(p.uid);
                status = ioctl(controller, DLP_IOCTL, &r) ? -errno : 0;
            } else if ((op == 2 || op == 3) && received < 0) {
                r.op = op == 2 ? 3 : 4;
                status = ioctl(controller, DLP_IOCTL, &r) ? -errno : 0;
                if (!status && op == 2) status = consume(controller, cookie, epoch);
                if (status == -ESTALE || status == -ENOENT) status = 0;

            }
        }
        if (received >= 0) close(received);
        uint32_t reply = htole32((uint32_t)status);
        if (send(peer, &reply, sizeof(reply), MSG_NOSIGNAL) != sizeof(reply)) break;
    }
out:
    if (controller >= 0) close(controller); /* revokes even on protocol error/death */
    close(peer);
}

int main(void)
{
    int listener = -1;
#ifdef __ANDROID__
    listener = android_get_control_socket("dagu_launch_profile");
#endif
    if (listener < 0 || listen(listener, 1)) {
        fprintf(stderr, "dagu_launch_profile: missing init socket\n");
        return 1;
    }
    for (;;) {
        int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (peer < 0 && errno == EINTR) continue;
        if (peer < 0) return 1;
        serve(peer);
    }
}
