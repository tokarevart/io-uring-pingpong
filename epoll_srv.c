/*
 * Competitive epoll baseline for the ping-pong benchmark.
 *
 * Deliberately tuned so the comparison is honest:
 *   - O_NONBLOCK everywhere, accept4(SOCK_NONBLOCK)
 *   - EPOLLET with full read draining until EAGAIN
 *   - partial writes are buffered and flushed via EPOLLOUT (never dropped)
 *   - fd -> slot indirection, so there is no artificial fd ceiling
 *   - deadline checked every N iterations to keep clock_gettime out of the
 *     hot path
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE 512
#define MAX_EVENTS 4096
#define DEADLINE_CHECK_EVERY 1024

struct client_state {
    int fd;
    int in_use;
    /* pending (possibly partial) echo */
    size_t echo_len;
    size_t echo_off;
    unsigned char echo[BUF_SIZE];
};

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

/* Stateless UDP echo loop -- the control for the io_uring UDP mode.
 *
 * Deliberately the same shape as the io_uring side: one socket, no per-client
 * state, nothing to set up beyond epoll_ctl. The only difference that matters
 * is the two syscalls per datagram, recvfrom() and sendto(), which is exactly
 * what the io_uring version replaces with a pair of SQEs.
 *
 * Counting matches the io_uring server: a transaction counts only when the
 * echo has actually gone out.
 */
static long long
run_udp_loop(int sock, int port, int duration_sec, long long msg_limit) {
    (void)port;
    const size_t bufsz = 64; /* must match MSG_SIZE in bench.sh */

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return 0;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = sock};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0) {
        perror("epoll_ctl add");
        close(epfd);
        return 0;
    }

    uint8_t *buf = malloc(bufsz);
    struct sockaddr_storage peer;
    if (!buf) {
        perror("malloc");
        close(epfd);
        return 0;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    long long completed = 0;
    long long errors = 0;
    unsigned ticks = 0;
    int stop = 0;

    while (!stop) {
        if (completed + errors >= msg_limit) {
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

        struct epoll_event evs[64];
        int n = epoll_wait(epfd, evs, 64, 200);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (!(evs[i].events & EPOLLIN)) {
                continue;
            }
            /* Drain: a burst of datagrams must not be left queued behind us. */
            for (;;) {
                socklen_t plen = sizeof(peer);
                ssize_t r =
                    recvfrom(sock, buf, bufsz, MSG_DONTWAIT, (struct sockaddr *)&peer, &plen);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    errors++;
                    break;
                }
                if ((size_t)r != bufsz) {
                    /* short or oversized datagram; never echo a partial reply */
                    errors++;
                    continue;
                }
                ssize_t s =
                    sendto(sock, buf, (size_t)r, MSG_DONTWAIT, (struct sockaddr *)&peer, plen);
                if (s == (ssize_t)r) {
                    completed++;
                } else {
                    errors++;
                }
            }
        }
    }

    fprintf(stderr, "epoll_srv(udp): completed=%lld errors=%lld\n", completed, errors);
    free(buf);
    close(epfd);
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

    /* UDP mode dispatches to a separate loop: it has no connections, so every
     * line below this point -- slots, fd indirection, partial writes, epoll_ctl
     * churn -- would be dead code. */
    int listen_fd = socket(AF_INET, is_udp ? SOCK_DGRAM : (SOCK_STREAM | SOCK_NONBLOCK), 0);
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
    if (is_udp) {
        long long done = run_udp_loop(listen_fd, port, duration_sec, msg_limit);
        (void)done;
        close(listen_fd);
        return 0;
    }

    if (!is_udp && listen(listen_fd, 4096) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(listen_fd);
        return 1;
    }

    struct epoll_event ev_listen = {.events = EPOLLIN, .data.fd = listen_fd};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev_listen) < 0) {
        perror("epoll_ctl add listen");
        return 1;
    }

    int max_fds = fd_limit();
    int *fd_slot = malloc((size_t)max_fds * sizeof(int));
    struct client_state *slots = calloc((size_t)max_fds, sizeof(struct client_state));
    int *free_list = malloc((size_t)max_fds * sizeof(int));
    if (!fd_slot || !slots || !free_list) {
        perror("malloc");
        return 1;
    }
    for (int i = 0; i < max_fds; i++) {
        fd_slot[i] = -1;
        free_list[i] = i;
    }
    int nfree = max_fds;

    struct epoll_event events[MAX_EVENTS];
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    long long processed = 0;
    unsigned ticks = 0;
    int running = 1;

    while (running && processed < msg_limit) {
        if (++ticks >= DEADLINE_CHECK_EVERY) {
            ticks = 0;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (duration_sec > 0 && now_sec(&now) - now_sec(&start) >= (double)duration_sec) {
                break;
            }
        }

        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                for (;;) {
                    int cfd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
                    if (cfd < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break; /* EAGAIN / EWOULDBLOCK / ENFILE */
                    }
                    if (cfd >= max_fds || nfree == 0) {
                        close(cfd);
                        continue;
                    }
                    int slot = free_list[nfree - 1];
                    nfree--;
                    fd_slot[cfd] = slot;
                    slots[slot].fd = cfd;
                    slots[slot].in_use = 1;
                    slots[slot].echo_len = 0;
                    slots[slot].echo_off = 0;

                    if (nodelay) {
                        int n = 1;
                        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
                    }

                    struct epoll_event cev = {.events = EPOLLIN | EPOLLET, .data.fd = cfd};
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
                        fd_slot[cfd] = -1;
                        slots[slot].in_use = 0;
                        free_list[nfree++] = slot;
                        close(cfd);
                    }
                }
                continue;
            }

            if (fd < 0 || fd >= max_fds) {
                continue;
            }
            int slot = fd_slot[fd];
            if (slot < 0 || !slots[slot].in_use) {
                continue;
            }
            struct client_state *c = &slots[slot];

            /* flush any pending echo first */
            int drop = 0;
            while (c->echo_off < c->echo_len) {
                ssize_t nw = write(fd, c->echo + c->echo_off, c->echo_len - c->echo_off);
                if (nw > 0) {
                    c->echo_off += (size_t)nw;
                    continue;
                }
                if (nw < 0 && errno == EINTR) {
                    continue;
                }
                if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break; /* keep buffered, EPOLLOUT will bring us back */
                }
                drop = 1;
                break;
            }

            if (!drop && c->echo_off == c->echo_len) {
                c->echo_len = 0;
                c->echo_off = 0;

                /* drain reads until EAGAIN */
                for (;;) {
                    ssize_t nread = read(fd, c->echo, BUF_SIZE);
                    if (nread > 0) {
                        size_t off = 0;
                        while (off < (size_t)nread) {
                            ssize_t nw = write(fd, c->echo + off, (size_t)nread - off);
                            if (nw > 0) {
                                off += (size_t)nw;
                                processed++;
                                continue;
                            }
                            if (nw < 0 && errno == EINTR) {
                                continue;
                            }
                            if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                /* stash the remainder */
                                memmove(c->echo, c->echo + off, (size_t)nread - off);
                                c->echo_len = (size_t)nread - off;
                                c->echo_off = 0;
                                drop = 0;
                                break;
                            }
                            drop = 1;
                            break;
                        }
                        if (drop || c->echo_len > 0) {
                            break;
                        }
                        if (processed >= msg_limit) {
                            break;
                        }
                        continue;
                    }
                    if (nread == 0) {
                        drop = 1;
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    drop = 1;
                    break;
                }
            }

            if (drop) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                fd_slot[fd] = -1;
                c->in_use = 0;
                c->echo_len = 0;
                c->echo_off = 0;
                free_list[nfree++] = slot;
                close(fd);
                continue;
            }

            /* switch interest depending on whether an echo is still owed */
            uint32_t want = EPOLLET;
            want |= (c->echo_len > c->echo_off) ? EPOLLOUT : EPOLLIN;
            if (events[i].events & (EPOLLOUT | EPOLLIN)) {
                struct epoll_event nev = {.events = want, .data.fd = fd};
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &nev);
            }
        }
    }

    fprintf(stderr, "epoll_srv: completed=%lld\n", processed);

    free(free_list);
    free(slots);
    free(fd_slot);
    close(epfd);
    close(listen_fd);
    return 0;
}
