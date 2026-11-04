/* SPDX-License-Identifier: Apache-2.0 */
/* Tests the production recvmsg parser with real seqpacket/SCM_RIGHTS syscalls. */
#define main helper_daemon_main
#include "../mi_mem_engine_helper.c"
#undef main
#include <assert.h>

static void send_packet(int fd, size_t length, int descriptor_count)
{
    struct packet p = { .version = htole32(1), .operation = htole32(1) };
    unsigned char body[64] = {0};
    memcpy(body, &p, sizeof(p));
    struct iovec iov = { .iov_base = body, .iov_len = length };
    char control[CMSG_SPACE(sizeof(int) * 2)] = {0};
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    int original = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(original >= 0);
    if (descriptor_count) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * descriptor_count);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * descriptor_count);
        int *fds = (int *)CMSG_DATA(c);
        for (int i = 0; i < descriptor_count; i++) fds[i] = original;
    }
    assert(sendmsg(fd, &msg, 0) == (ssize_t)length);
    close(original);
}
int main(void)
{
    int pair[2], received;
    struct packet p;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    send_packet(pair[0], 32, 1);
    assert(receive(pair[1], &p, &received) == 0);
    assert(received >= 0 && (fcntl(received, F_GETFD) & FD_CLOEXEC));
    close(received);
    send_packet(pair[0], 32, 2);
    assert(receive(pair[1], &p, &received) == -1 && received == -1);
    send_packet(pair[0], 64, 1);
    assert(receive(pair[1], &p, &received) == -1 && received == -1);
    send_packet(pair[0], 8, 0);
    assert(receive(pair[1], &p, &received) == -1 && received == -1);
    close(pair[0]);
    assert(receive(pair[1], &p, &received) == -1 && received == -1);
    close(pair[1]);
    puts("production seqpacket parser: valid FD, excess FD, truncation, short packet, EOF passed");
    return 0;
}
