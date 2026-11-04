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
#include <linux/mi_mem_engine.h>
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

static struct mme_request request(uint64_t cookie)
{
    struct mme_request r = { .version = MME_VERSION, .size = sizeof(r), .cookie = cookie };
    return r;
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
    controller = open("/dev/mi_mem_engine", O_RDWR | O_CLOEXEC);
    if (controller < 0) goto out;
    struct mme_request enable = request(0);
    enable.lease_ms = 1;
    if (ioctl(controller, MME_ENABLE, &enable)) goto out;
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
        struct mme_request r = request(cookie);
        int status = -EINVAL;
        if (le32toh(p.version) == 1 && cookie) {
            if (op == 1 && received >= 0 && le32toh(p.lease_ms) > 0
                    && le32toh(p.lease_ms) <= MME_MAX_LEASE_MS) {
                r.nr_tasks = 1;
                r.lease_ms = le32toh(p.lease_ms);
                r.tasks[0].binder_fd = received;
                r.tasks[0].binder_handle = le32toh(p.binder_handle);
                r.tasks[0].tid = le32toh(p.tid);
                r.tasks[0].uid = le32toh(p.uid);
                status = ioctl(controller, MME_BEGIN, &r) ? -errno : 0;
            } else if (op == 2 && received < 0) {
                status = ioctl(controller, MME_END, &r) ? -errno : 0;
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
    listener = android_get_control_socket("mi_mem_engine");
#endif
    if (listener < 0 || listen(listener, 1)) {
        fprintf(stderr, "mi_mem_engine: missing init socket\n");
        return 1;
    }
    for (;;) {
        int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (peer < 0 && errno == EINTR) continue;
        if (peer < 0) return 1;
        serve(peer);
    }
}
