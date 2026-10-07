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
#include <time.h>
#include <unistd.h>

#define ENTRIES 4096
#define BUF_BGID 1
#define BUF_NUM 2048
#define BUF_SIZE 512
#define DEADLINE_CHECK_EVERY 1024

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
setup_buffer_ring(struct io_uring *ring, uint8_t *buf_space) {
    int err = 0;
    struct io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, BUF_NUM, BUF_BGID, 0, &err);
    if (!br) {
        fprintf(stderr, "io_uring_setup_buf_ring failed: %s\n", strerror(-err));
        return NULL;
    }
    for (int i = 0; i < BUF_NUM; i++) {
        io_uring_buf_ring_add(
            br,
            &buf_space[(size_t)i * BUF_SIZE],
            BUF_SIZE,
            (unsigned int)(i + 1), /* buf_id is 1-based */
            io_uring_buf_ring_mask(BUF_NUM),
            (unsigned short)i
        );
    }
    io_uring_buf_ring_advance(br, BUF_NUM);
    return br;
}

int
main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <duration_sec> <msg_count_limit> [nodelay=1]\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    int duration_sec = atoi(argv[2]);
    long long msg_limit = atoll(argv[3]);
    int nodelay = (argc >= 5) ? atoi(argv[4]) : 1;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
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
    if (listen(listen_fd, 4096) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

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

    alignas(4096) uint8_t buf_space[BUF_NUM * BUF_SIZE];
    struct io_uring_buf_ring *br = setup_buffer_ring(&ring, buf_space);
    if (!br) {
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 1;
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

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

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

        io_uring_submit_and_wait(&ring, 1);

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
