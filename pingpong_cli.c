/*
 * Multi-process, poll()-driven ping-pong load generator.
 *
 * The client must never be the bottleneck, so it forks N worker processes,
 * each owning a disjoint slice of connections and each pinned to its own
 * logical CPU. poll() is used instead of select() to lift the FD_SETSIZE
 * (1024) ceiling that otherwise caps the connection sweep.
 *
 * Strict ping-pong per connection: send one request, block until the echo
 * of exactly that request comes back, then send the next. That keeps at most
 * one request in flight per connection, so server-side accounting is
 * unambiguous.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct conn {
    int fd;
    int awaiting; /* request sent, echo outstanding */
    long long sent;
};

struct worker_cfg {
    struct sockaddr_in addr;
    int first;
    int count;
    long long msgs_per_conn;
    int msg_size;
    int nodelay;
    int cpu;
    double duration;
};

/* Handed back to the parent over the pipe. */
struct worker_result {
    long long completed;
    long long elapsed_ns;
    int connected;
    int requested;
};

static void
usage(const char *prog) {
    fprintf(
        stderr,
        "usage: %s <host> <port> <conns> <msgs_per_conn> <msg_size>\n"
        "          [duration_sec] [nodelay=1] [workers=auto] [cpu_base=1]\n"
        "\n"
        "Prints a single line: '<total_completed> <measurement_seconds>'.\n"
        "The measurement window starts only after every connection is\n"
        "established, so connection setup is excluded from the reported rate.\n"
        "msgs_per_conn < 0 means run until the duration expires.\n"
        "workers are pinned to cpus cpu_base, cpu_base+1, ...\n",
        prog
    );
}

static double
now_sec(const struct timespec *ts) {
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

static int
pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/* Non-blocking connect that waits for writability, so N connections can be
 * established quickly without serialising on the handshake. */
static int
connect_nb(const struct sockaddr_in *addr, int nodelay) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    if (nodelay) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    /* Connects are issued sequentially, so this per-connect timeout is the
     * worst-case cost of a stalled one. Keep it short: with ~1024 connects per
     * worker a 10s timeout would mean hours of dead time. */
    struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
    int rc = poll(&pfd, 1, 2000);
    if (rc <= 0) {
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
run_worker(const struct worker_cfg *cfg, struct worker_result *out) {
    struct conn *conns = calloc((size_t)cfg->count, sizeof(*conns));
    char *req = malloc((size_t)cfg->msg_size);
    char *echo = malloc((size_t)cfg->msg_size);
    struct pollfd *pfds = calloc((size_t)cfg->count, sizeof(*pfds));
    if (!conns || !req || !echo || !pfds) {
        return;
    }
    memset(req, 'A', (size_t)cfg->msg_size);
    memset(echo, 0, (size_t)cfg->msg_size);

    int live = 0;
    for (int i = 0; i < cfg->count; i++) {
        int fd = connect_nb(&cfg->addr, cfg->nodelay);
        if (fd < 0) {
            continue;
        }
        conns[live].fd = fd;
        conns[live].awaiting = 0;
        conns[live].sent = 0;
        live++;
    }

    /* count established at this point; `live` shrinks again as conns die */
    int established = live;

    /* The measurement window starts here, not before: time spent establishing
     * sockets produced zero transactions, and at high connection counts it is
     * a large fraction of the window. Including it would deflate the reported
     * rate in proportion to how many connections were opened. */
    struct timespec ready;
    clock_gettime(CLOCK_MONOTONIC, &ready);

    long long completed = 0;
    long long per_conn_limit = cfg->msgs_per_conn;
    int infinite = (per_conn_limit < 0);

    while (live > 0) {
        struct timespec nowts;
        clock_gettime(CLOCK_MONOTONIC, &nowts);
        if (now_sec(&nowts) - now_sec(&ready) >= cfg->duration) {
            break;
        }

        int n = 0;
        for (int i = 0; i < live; i++) {
            pfds[n].fd = conns[i].fd;
            pfds[n].events = conns[i].awaiting ? POLLIN : POLLOUT;
            pfds[n].revents = 0;
            n++;
        }

        int rc = poll(pfds, (nfds_t)n, 50);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        int out = 0;
        for (int i = 0; i < live; i++) {
            short re = pfds[i].revents;
            if (re == 0) {
                conns[out++] = conns[i];
                continue;
            }

            int dead = 0;
            if (conns[i].awaiting && (re & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t nread = recv(conns[i].fd, echo, (size_t)cfg->msg_size, 0);
                if (nread == cfg->msg_size) {
                    completed++;
                    if (!infinite && conns[i].sent >= per_conn_limit) {
                        /* this connection has served its budget */
                        dead = 1;
                    }
                } else if (nread <= 0) {
                    dead = 1;
                }
                if (!dead) {
                    conns[i].awaiting = 0;
                }
            } else if (!conns[i].awaiting && (re & POLLOUT)) {
                if (!infinite && conns[i].sent >= per_conn_limit) {
                    dead = 1;
                } else {
                    ssize_t nw = send(conns[i].fd, req, (size_t)cfg->msg_size, 0);
                    if (nw == cfg->msg_size) {
                        conns[i].awaiting = 1;
                        conns[i].sent++;
                    } else if (nw < 0 && (errno == EAGAIN || errno == EINTR)) {
                        /* retry next round */
                    } else {
                        dead = 1;
                    }
                }
            } else if (re & (POLLHUP | POLLERR | POLLNVAL)) {
                dead = 1;
            }

            if (dead) {
                close(conns[i].fd);
            } else {
                conns[out++] = conns[i];
            }
        }
        live = out;
    }

    for (int i = 0; i < live; i++) {
        close(conns[i].fd);
    }

    struct timespec done;
    clock_gettime(CLOCK_MONOTONIC, &done);
    out->completed = completed;
    out->elapsed_ns =
        (long long)((done.tv_sec - ready.tv_sec) * 1000000000LL + (done.tv_nsec - ready.tv_nsec));
    out->connected = established;
    out->requested = cfg->count;

    free(pfds);
    free(echo);
    free(req);
    free(conns);
}

int
main(int argc, char **argv) {
    if (argc < 6) {
        usage(argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    int conns = atoi(argv[3]);
    long long msgs_per_conn = atoll(argv[4]);
    int msg_size = atoi(argv[5]);
    int duration = (argc >= 7) ? atoi(argv[6]) : 10;
    int nodelay = (argc >= 8) ? atoi(argv[7]) : 1;
    int workers = (argc >= 9) ? atoi(argv[8]) : 0;
    int cpu_base = (argc >= 10) ? atoi(argv[9]) : 1;

    if (conns <= 0 || msg_size <= 0) {
        usage(argv[0]);
        return 1;
    }

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) {
        ncpu = 1;
    }
    int cpus_avail = (int)ncpu - cpu_base;
    if (cpus_avail < 1) {
        cpus_avail = 1;
    }
    if (workers <= 0) {
        /* One worker per available cpu, but never more workers than conns. */
        workers = cpus_avail;
        if (workers > conns) {
            workers = conns;
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid host: %s\n", host);
        return 1;
    }

    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t *pids = calloc((size_t)workers, sizeof(*pids));
    struct worker_cfg *cfgs = calloc((size_t)workers, sizeof(*cfgs));
    if (!pids || !cfgs) {
        perror("calloc");
        return 1;
    }

    int per = conns / workers;
    int extra = conns % workers;

    for (int w = 0; w < workers; w++) {
        cfgs[w].addr = addr;
        cfgs[w].first = w * per;
        cfgs[w].count = per + (w < extra ? 1 : 0);
        cfgs[w].msgs_per_conn = msgs_per_conn;
        cfgs[w].msg_size = msg_size;
        cfgs[w].nodelay = nodelay;
        cfgs[w].cpu = cpu_base + w;
        cfgs[w].duration = (double)duration;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            close(pfd[0]);
            pin_to_cpu(cfgs[w].cpu);
            struct worker_result r;
            memset(&r, 0, sizeof(r));
            run_worker(&cfgs[w], &r);
            ssize_t ignored = write(pfd[1], &r, sizeof(r));
            (void)ignored;
            _exit(0);
        }
        pids[w] = pid;
    }

    close(pfd[1]);

    long long total = 0;
    long long max_elapsed_ns = 0;
    int connected = 0;
    int requested = 0;
    for (int w = 0; w < workers; w++) {
        struct worker_result r;
        ssize_t got = read(pfd[0], &r, sizeof(r));
        if (got == (ssize_t)sizeof(r)) {
            total += r.completed;
            connected += r.connected;
            requested += r.requested;
            if (r.elapsed_ns > max_elapsed_ns) {
                max_elapsed_ns = r.elapsed_ns;
            }
        }
    }
    close(pfd[0]);
    for (int w = 0; w < workers; w++) {
        waitpid(pids[w], NULL, 0);
    }

    if (connected < requested) {
        fprintf(
            stderr,
            "warning: only %d/%d connections established; the reported rate is "
            "for %d connections, not %d\n",
            connected,
            requested,
            connected,
            requested
        );
    }

    /* Workers begin their windows within milliseconds of each other (all of
     * them connect concurrently), so the longest window is the honest
     * denominator for the summed transaction count. */
    double elapsed = (double)max_elapsed_ns / 1e9;

    printf("%lld %.6f\n", total, elapsed);
    free(cfgs);
    free(pids);
    return 0;
}
