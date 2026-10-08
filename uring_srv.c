/*
 * Optimised io_uring ping-pong echo server.
 *
 * Wins over epoll only if it actually uses the features epoll lacks:
 *   - IORING_SETUP_SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN to keep
 *     completion handling in the submitter's context
 *   - one multishot accept SQE for the whole listener (no epoll_ctl churn)
 *   - one multishot recv SQE per connection with IOSQE_BUFFER_SELECT
 *   - a provided buffer ring, so reads need no per-connection allocation
 *   - batched CQE draining with a single io_uring_enter per wakeup
 *
 * user_data layout (op tag protects against stale completions after an fd
 * is closed and its number recycled by a later accept):
 *
 *   bits 63..62  op    (0 accept, 1 recv, 2 send)
 *   bits 61..40  gen   connection generation (22 bits)
 *   bits 39..20  fd
 *   bits 19..0   buf_id (send only)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define ENTRIES 4096
#define BUF_BGID 1
#define BUF_NUM 2048
/* Buffer size must be >= the client's payload, because the multishot recv is
 * prepared with len=0 ("no limit") and the provided buffer's io_len is what
 * actually caps the transfer. A smaller buffer truncates: a 128-byte request
 * arrives as two 64-byte echoes, so the client reads a 64-byte reply that does
 * not match what it sent.
 *
 * That failure is caught -- the client's echo tag check reports roughly half the
 * echoes mismatched and aborts the run -- but it presents as content
 * corruption rather than as the sizing mistake it actually is, which is a
 * confusing way to learn about it. So the sizes are pinned instead: 64 here,
 * MSG_SIZE=64 in bench.sh.
 *
 * There is no throughput argument for 64 over a larger size either way (see
 * RESULTS.md), so this is purely about making a wrong configuration legible.
 */
#define BUF_SIZE 64
#define DEADLINE_CHECK_EVERY 1024

/* UDP mode. Datagrams carry no connection state, so the server is stateless:
 * one socket, one permanently-armed multishot recvmsg, and every echo is sent
 * back to the sockaddr the kernel hands us. Nothing per-client is kept, which
 * is why this mode has no accept, no generation counter and no close path.
 *
 * The catch is that multishot recvmsg cannot return a bare payload: the kernel
 * prepends the aux layout (recvmsg_out header + the source sockaddr) *inside*
 * the provided buffer. So a UDP buffer is AUX bytes bigger than the payload,
 * and the echo length must come from the aux payloadlen field -- cqe->res is
 * aux+payload together, not the payload length.
 *
 * PAYLOAD_SIZE must match MSG_SIZE in bench.sh, for the same reason BUF_SIZE
 * must: a larger datagram is truncated silently by recv, and MSG_TRUNC would
 * then have to be caught per datagram rather than by construction.
 */
#define PAYLOAD_SIZE 64
#define UDP_AUX (sizeof(struct io_uring_recvmsg_out) + sizeof(struct sockaddr_in))
#define UDP_BUF_SIZE (UDP_AUX + PAYLOAD_SIZE)
#define MAX_BUF_SIZE (UDP_BUF_SIZE > BUF_SIZE ? UDP_BUF_SIZE : BUF_SIZE)

#define OP_ACCEPT 0ULL
#define OP_RECV 1ULL
#define OP_SEND 2ULL

#define UD_MAKE(op, gen, fd, buf)                                       \
    (((uint64_t)(op) << 62) | (((uint64_t)(gen) & 0x3FFFFFULL) << 40) | \
     (((uint64_t)(fd) & 0xFFFFFULL) << 20) | ((uint64_t)(buf) & 0xFFFFFULL))

#define UD_OP(ud) (((ud) >> 62) & 0x3ULL)
#define UD_GEN(ud) (((ud) >> 40) & 0x3FFFFFULL)
#define UD_FD(ud) ((int)(((ud) >> 20) & 0xFFFFFULL))
#define UD_BUF(ud) ((unsigned int)((ud) & 0xFFFFFULL))

static int
fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0 &&
        rl.rlim_cur < (rlim_t)(1 << 20)) {
        return (int)rl.rlim_cur;
    }
    return 1 << 16;
}

static double
now_sec(const struct timespec *ts) {
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

static struct io_uring_buf_ring *
setup_buffer_ring(struct io_uring *ring, uint8_t *buf_space, size_t bstride) {
    int err = 0;
    struct io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, BUF_NUM, BUF_BGID, 0, &err);
    if (!br) {
        fprintf(stderr, "io_uring_setup_buf_ring failed: %s\n", strerror(-err));
        return NULL;
    }
    for (int i = 0; i < BUF_NUM; i++) {
        io_uring_buf_ring_add(
            br,
            &buf_space[(size_t)i * bstride],
            bstride,
            (unsigned int)(i + 1), /* buf_id is 1-based */
            io_uring_buf_ring_mask(BUF_NUM),
            (unsigned short)i
        );
    }
    io_uring_buf_ring_advance(br, BUF_NUM);
    return br;
}

/* Return a buffer to the ring. Kept in one place because the ownership rule
 * depends on it: a buffer is ours from the recv completion until the send that
 * reads from it completes, and not one microsecond earlier. */
static void
recycle_buf(struct io_uring_buf_ring *br, uint8_t *buf_space, size_t bstride, unsigned int buf_id) {
    if (buf_id < 1 || buf_id > BUF_NUM) {
        return;
    }
    io_uring_buf_ring_add(
        br,
        &buf_space[(size_t)(buf_id - 1) * bstride],
        bstride,
        buf_id,
        io_uring_buf_ring_mask(BUF_NUM),
        0
    );
    io_uring_buf_ring_advance(br, 1);
}

/* Stateless UDP echo loop.
 *
 * One multishot recvmsg stays armed for the whole run. Each completion carries a
 * buffer whose layout is [recvmsg_out][sockaddr][payload]; the payload is echoed
 * back to that sockaddr with a sendmsg whose msghdr/iov live in per-buf_id slots,
 * so an in-flight echo owns its own descriptor exactly as it owns its buffer.
 *
 * Anything unexpected -- a truncated datagram, an unexpected payload length, a
 * lost buffer group -- is counted and *not* echoed, rather than echoed as
 * garbage. A benchmark that quietly served short replies would still report a
 * healthy rps.
 */
static long long
run_udp_loop(
    struct io_uring *ring,
    struct io_uring_buf_ring *br,
    int sock,
    uint8_t *buf_space,
    size_t bstride,
    int duration_sec,
    long long msg_limit,
    const struct timespec *start
) {
    /* Describes the aux layout the kernel writes: a sockaddr_in of name space,
     * no control space. The iov is unused with buffer select but must be valid. */
    struct sockaddr_in *name_space = calloc(1, sizeof(struct sockaddr_in));
    struct iovec *recv_iov = calloc(1, sizeof(struct iovec));
    struct msghdr *recv_msg = calloc(1, sizeof(struct msghdr));
    /* Per-buf_id send descriptors. Exclusive to that buffer while its echo is
     * in flight, which is the same ownership window the buffer itself has. */
    struct msghdr *send_msg = calloc(BUF_NUM, sizeof(struct msghdr));
    struct iovec *send_iov = calloc(BUF_NUM, sizeof(struct iovec));
    uint8_t *scratch = calloc(1, bstride);
    if (!name_space || !recv_iov || !recv_msg || !send_msg || !send_iov || !scratch) {
        perror("calloc");
        return 0;
    }
    recv_iov->iov_base = scratch;
    recv_iov->iov_len = bstride;
    recv_msg->msg_name = name_space;
    recv_msg->msg_namelen = sizeof(struct sockaddr_in);
    recv_msg->msg_iov = recv_iov;
    recv_msg->msg_iovlen = 1;

    long long completed = 0;
    long long errors = 0;
    unsigned ticks = 0;
    /* The recvmsg is armed once and then re-armed only if the multishot ends.
     * Start armed: there is no accept to do it for us here. */
    int rearm = 1;

    for (;;) {
        if (completed + errors >= msg_limit) {
            break;
        }
        if (++ticks >= DEADLINE_CHECK_EVERY) {
            ticks = 0;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (duration_sec > 0 && now_sec(&now) - now_sec(start) >= (double)duration_sec) {
                break;
            }
        }

        if (rearm) {
            struct io_uring_sqe *rs = io_uring_get_sqe(ring);
            if (rs) {
                io_uring_prep_recvmsg_multishot(rs, sock, recv_msg, 0);
                rs->flags |= IOSQE_BUFFER_SELECT;
                rs->buf_group = BUF_BGID;
                rs->user_data = UD_MAKE(OP_RECV, 0, 0, 0);
                rearm = 0;
            }
        }

        int sub = io_uring_submit_and_wait(ring, 1);
        if (sub < 0 && sub != -EINTR) {
            /* Fatal, not merely interrupted. -EINTR must NOT end the run: under
             * strace the enter comes back interrupted constantly, and treating
             * that as fatal made the server exit before serving anything. */
            break;
        }

        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe *cqe;
        io_uring_for_each_cqe(ring, head, cqe) {
            count++;
            uint64_t ud = cqe->user_data;
            int op = (int)UD_OP(ud);

            if (op == OP_SEND) {
                unsigned int buf_id = UD_BUF(ud);
                recycle_buf(br, buf_space, bstride, buf_id);
                if (cqe->res < 0) {
                    errors++;
                } else {
                    completed++;
                }
                continue;
            }

            /* OP_RECV */
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                rearm = 1; /* multishot ended: ENOBUFS or a fatal error */
            }
            if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
                errors++;
                continue;
            }
            unsigned int buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            if (cqe->res < 0 || buf_id < 1 || buf_id > BUF_NUM) {
                errors++;
                recycle_buf(br, buf_space, bstride, buf_id);
                continue;
            }

            uint8_t *b = &buf_space[(size_t)(buf_id - 1) * bstride];
            struct io_uring_recvmsg_out *o = (struct io_uring_recvmsg_out *)b;

            int unusable = (o->flags & MSG_TRUNC) || o->namelen != sizeof(struct sockaddr_in) ||
                           o->payloadlen != PAYLOAD_SIZE;
            if (unusable) {
                /* Would mean MSG_SIZE and PAYLOAD_SIZE have drifted apart, or a
                 * datagram arrived that this buffer cannot hold. Count it and
                 * drop it; never echo a short reply. */
                errors++;
                recycle_buf(br, buf_space, bstride, buf_id);
                continue;
            }

            struct io_uring_sqe *ss = io_uring_get_sqe(ring);
            if (!ss) {
                errors++;
                recycle_buf(br, buf_space, bstride, buf_id);
                continue;
            }
            send_iov[buf_id - 1].iov_base = io_uring_recvmsg_payload(o, recv_msg);
            send_iov[buf_id - 1].iov_len = o->payloadlen;
            memset(&send_msg[buf_id - 1], 0, sizeof(send_msg[0]));
            send_msg[buf_id - 1].msg_name = io_uring_recvmsg_name(o);
            send_msg[buf_id - 1].msg_namelen = o->namelen;
            send_msg[buf_id - 1].msg_iov = &send_iov[buf_id - 1];
            send_msg[buf_id - 1].msg_iovlen = 1;
            io_uring_prep_sendmsg(ss, sock, &send_msg[buf_id - 1], 0);
            ss->user_data = UD_MAKE(OP_SEND, 0, 0, buf_id);
            /* the buffer is recycled when the send completes, not now */
        }
        io_uring_cq_advance(ring, count);
    }

    fprintf(stderr, "uring_srv(udp): completed=%lld errors=%lld\n", completed, errors);
    free(name_space);
    free(recv_iov);
    free(recv_msg);
    free(send_msg);
    free(send_iov);
    free(scratch);
    return completed;
}

int
main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(
            stderr,
            "usage: %s <port> <duration_sec> <msg_count_limit> [nodelay=1] [proto=tcp]\n",
            argv[0]
        );
        return 1;
    }

    int port = atoi(argv[1]);
    /* Both sides bind with htons((uint16_t)port), so a port above 65535 is
     * silently truncated rather than rejected. Client and server would truncate
     * identically and the run would appear to work, while the harness's port
     * checks looked for a port nobody ever bound. Fail loudly instead. */
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "port %d out of range (1-65535)\n", port);
        return 1;
    }
    int duration_sec = atoi(argv[2]);
    long long msg_limit = atoll(argv[3]);
    int nodelay = (argc >= 5) ? atoi(argv[4]) : 1;
    const char *proto = (argc >= 6) ? argv[5] : "tcp";
    int is_udp;
    if (strcmp(proto, "udp") == 0) {
        is_udp = 1;
    } else if (strcmp(proto, "tcp") == 0) {
        is_udp = 0;
    } else {
        fprintf(stderr, "unknown proto '%s' (want tcp or udp)\n", proto);
        return 1;
    }

    /* UDP is datagram-based, so nodelay has no meaning and is silently ignored
     * rather than failing a run that did not ask for it. */
    int listen_fd = socket(AF_INET, is_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    /* A listening stream socket is what feeds multishot accept; a UDP socket is
     * already receiving, so there is nothing to listen for. */
    if (!is_udp && listen(listen_fd, 4096) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    size_t bstride = is_udp ? UDP_BUF_SIZE : BUF_SIZE;

    struct io_uring ring;
    struct io_uring_params params = {0};
    params.flags =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

    int rc = io_uring_queue_init_params(ENTRIES, &ring, &params);
    if (rc < 0) {
        fprintf(stderr, "io_uring_queue_init_params: %s\n", strerror(-rc));
        close(listen_fd);
        return 1;
    }

    alignas(4096) uint8_t buf_space[BUF_NUM * MAX_BUF_SIZE];
    struct io_uring_buf_ring *br = setup_buffer_ring(&ring, buf_space, bstride);
    if (!br) {
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 1;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (is_udp) {
        long long done =
            run_udp_loop(&ring, br, listen_fd, buf_space, bstride, duration_sec, msg_limit, &start);
        (void)done;
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 0;
    }

    int max_fds = fd_limit();
    uint32_t *fd_gen = calloc((size_t)max_fds, sizeof(uint32_t));
    if (!fd_gen) {
        perror("calloc");
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "no sqe for accept\n");
        return 1;
    }
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    sqe->user_data = UD_MAKE(OP_ACCEPT, 0, 0, 0);
    io_uring_submit(&ring);

    long long completed_responses = 0;
    unsigned ticks = 0;

    for (;;) {
        if (completed_responses >= msg_limit) {
            break;
        }
        if (++ticks >= DEADLINE_CHECK_EVERY) {
            ticks = 0;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (duration_sec > 0 && now_sec(&now) - now_sec(&start) >= (double)duration_sec) {
                break;
            }
        }

        int sub = io_uring_submit_and_wait(&ring, 1);
        if (sub < 0 && sub != -EINTR) {
            break; /* fatal only; see the udp loop */
        }

        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe *cqe;
        io_uring_for_each_cqe(&ring, head, cqe) {
            count++;
            uint64_t ud = cqe->user_data;
            int op = (int)UD_OP(ud);

            if (op == OP_SEND) {
                int fd = UD_FD(ud);
                uint32_t gen = (uint32_t)UD_GEN(ud);
                unsigned int buf_id = UD_BUF(ud);

                /* the buffer always comes back to the ring, success or not */
                if (buf_id >= 1 && buf_id <= BUF_NUM) {
                    io_uring_buf_ring_add(
                        br,
                        &buf_space[(size_t)(buf_id - 1) * BUF_SIZE],
                        BUF_SIZE,
                        buf_id,
                        io_uring_buf_ring_mask(BUF_NUM),
                        0
                    );
                    io_uring_buf_ring_advance(br, 1);
                }

                if (fd < 0 || fd >= max_fds || fd_gen[fd] != gen) {
                    continue; /* stale: fd closed and possibly recycled */
                }
                if (cqe->res < 0) {
                    fd_gen[fd]++;
                    close(fd);
                } else {
                    completed_responses++;
                }
                continue;
            }

            if (op == OP_ACCEPT) {
                if (cqe->res < 0) {
                    continue; /* transient (ENFILE, ECANCELED, ...) */
                }
                int cfd = cqe->res;
                if (cfd >= max_fds) {
                    close(cfd);
                    continue;
                }
                if (nodelay) {
                    int n = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
                }
                uint32_t gen = ++fd_gen[cfd];

                struct io_uring_sqe *rs = io_uring_get_sqe(&ring);
                if (!rs) {
                    fd_gen[cfd]++;
                    close(cfd);
                    continue;
                }
                io_uring_prep_recv_multishot(rs, cfd, NULL, 0, 0);
                rs->flags |= IOSQE_BUFFER_SELECT;
                rs->buf_group = BUF_BGID;
                rs->user_data = UD_MAKE(OP_RECV, gen, cfd, 0);
                continue;
            }

            /* OP_RECV */
            int fd = UD_FD(ud);
            uint32_t gen = (uint32_t)UD_GEN(ud);
            int nread = cqe->res;
            int buf_id = -1;
            if (cqe->flags & IORING_CQE_F_BUFFER) {
                buf_id = (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            }

            if (fd < 0 || fd >= max_fds || fd_gen[fd] != gen) {
                /* stale completion: still owe the buffer back to the ring */
                if (buf_id >= 1 && buf_id <= BUF_NUM) {
                    io_uring_buf_ring_add(
                        br,
                        &buf_space[(size_t)(buf_id - 1) * BUF_SIZE],
                        BUF_SIZE,
                        (unsigned int)buf_id,
                        io_uring_buf_ring_mask(BUF_NUM),
                        0
                    );
                    io_uring_buf_ring_advance(br, 1);
                }
                continue;
            }

            if (nread <= 0 || buf_id < 1 || buf_id > BUF_NUM) {
                /* EOF, error, or no buffer selected: buffer is not ours */
                fd_gen[fd]++;
                close(fd);
                continue;
            }

            struct io_uring_sqe *ss = io_uring_get_sqe(&ring);
            if (!ss) {
                /* cannot echo; the buffer is still owned by us, give it back */
                io_uring_buf_ring_add(
                    br,
                    &buf_space[(size_t)(buf_id - 1) * BUF_SIZE],
                    BUF_SIZE,
                    (unsigned int)buf_id,
                    io_uring_buf_ring_mask(BUF_NUM),
                    0
                );
                io_uring_buf_ring_advance(br, 1);
                fd_gen[fd]++;
                close(fd);
                continue;
            }
            io_uring_prep_send(
                ss, fd, &buf_space[(size_t)(buf_id - 1) * BUF_SIZE], (size_t)nread, 0
            );
            ss->user_data = UD_MAKE(OP_SEND, gen, fd, (unsigned int)buf_id);
            /* the buffer is recycled when the send completes, not now */
        }

        /* advance by how many CQEs we consumed -- NOT by the head index */
        io_uring_cq_advance(&ring, count);
    }

    fprintf(stderr, "uring_srv: completed=%lld\n", completed_responses);

    free(fd_gen);
    io_uring_queue_exit(&ring);
    close(listen_fd);
    return 0;
}
