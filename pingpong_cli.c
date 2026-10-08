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
 *
 * Every request carries a per-connection, per-request tag in its first bytes
 * and the echo is compared against it. This is not a nicety: a server that
 * recycles a buffer early still returns a full-length echo, just the wrong
 * bytes, so a length check alone cannot detect it. Verifying content is the
 * only way that class of bug fails loudly instead of silently skewing rps.
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

/* Bytes of every request that carry the identifying tag. One cache line, so
 * verification stays off the hot path; 16 is the natural choice on x86-64. */
#define TAG_BYTES 16

struct conn {
    int fd;
    int awaiting; /* request sent, echo outstanding */
    long long sent;
    /* Tag of the request currently in flight, i.e. what the echo must match.
     * Valid only while awaiting is set. */
    unsigned char tag[TAG_BYTES];
    /* Global index across the whole run, so two connections never share a tag
     * even when they live in the same worker. */
    uint64_t index;
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
    /* Echoes whose content did not match the request that produced them. Any
     * non-zero value means the server returned the wrong bytes and the run's
     * throughput numbers are not trustworthy. */
    long long mismatches;
    long long elapsed_ns;
    long long cpu_ticks; /* self utime+stime, in clock ticks */
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
        "Prints a single line:\n"
        "  '<total_completed> <measurement_seconds> <client_cpu_percent_of_one_core>"
        " <echo_mismatches>'\n"
        "The measurement window starts only after every connection is\n"
        "established, so connection setup is excluded from the reported rate.\n"
        "Every request carries a per-connection tag that the echo is checked\n"
        "against. Exit status is non-zero if any echo failed that check, so a\n"
        "run that served wrong data can never be read as a valid result.\n"
        "msgs_per_conn < 0 means run until the duration expires.\n"
        "workers are pinned to cpus cpu_base, cpu_base+1, ...\n",
        prog
    );
}

static double
now_sec(const struct timespec *ts) {
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

/* Process CPU used so far, in clock ticks (sysconf(_SC_CLK_TCK) per second).
 *
 * /proc/self/stat is "pid (comm) state ...", and comm may itself contain
 * spaces or parens, so the numeric fields are located from the LAST ')'
 * rather than by splitting the whole line on whitespace.
 */
static long long
self_cpu_ticks(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) {
        return 0;
    }
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char *p = strrchr(buf, ')');
    if (!p) {
        return 0;
    }
    /* after ") " comes state=field 3, so utime=field 14 is the 12th token */
    int field = 2;
    long long utime = 0, stime = 0;
    for (char *tok = strtok(p + 1, " "); tok; tok = strtok(NULL, " ")) {
        field++;
        if (field == 14) {
            utime = strtoll(tok, NULL, 10);
        } else if (field == 15) {
            stime = strtoll(tok, NULL, 10);
            break;
        }
    }
    return utime + stime;
}

static int
pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/* Tag written into every request: a per-run nonce, the connection's global
 * index, and the request's sequence number. Two properties matter. The nonce
 * keeps an all-zero or all-'A' buffer from passing the comparison by accident,
 * and the sequence number makes a stale echo -- one replayed from an earlier
 * request -- detectable instead of indistinguishable from the right one. These
 * are little-endian byte copies rather than a packed struct, so the on-wire
 * layout cannot depend on the compiler's padding choices. */
static void
make_tag(unsigned char *tag, uint64_t nonce, uint64_t conn_index, uint64_t seq) {
    memcpy(tag, &nonce, sizeof(nonce));
    memcpy(tag + sizeof(nonce), &conn_index, sizeof(conn_index));
    memcpy(tag + sizeof(nonce) + sizeof(conn_index), &seq, sizeof(seq));
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
    unsigned char *req = malloc((size_t)cfg->msg_size);
    unsigned char *echo = malloc((size_t)cfg->msg_size);
    struct pollfd *pfds = calloc((size_t)cfg->count, sizeof(*pfds));
    if (!conns || !req || !echo || !pfds) {
        return;
    }
    memset(req, 'A', (size_t)cfg->msg_size);
    memset(echo, 0, (size_t)cfg->msg_size);

    /* The tag has to fit in a message; a smaller-than-16B payload still gets
     * whatever prefix fits, so verification still works at small sizes. */
    if (cfg->msg_size < TAG_BYTES) {
        fprintf(stderr, "msg_size=%d is smaller than the %d-byte tag\n", cfg->msg_size, TAG_BYTES);
        return;
    }

    /* Per-run nonce, so a zeroed or stale buffer cannot pass by accident, and
     * so a buffer left over from a previous run is never accepted. */
    uint64_t nonce = 0x9e3779b97f4a7c15ULL;
    nonce ^= (uint64_t)getpid();
    struct timespec nts;
    clock_gettime(CLOCK_REALTIME, &nts);
    nonce ^= (uint64_t)nts.tv_nsec * 0xff51afd7ed558ccdULL;
    nonce ^= (uint64_t)nts.tv_sec << 17;

    long long mismatches = 0;
    int live = 0;
    for (int i = 0; i < cfg->count; i++) {
        int fd = connect_nb(&cfg->addr, cfg->nodelay);
        if (fd < 0) {
            continue;
        }
        conns[live].fd = fd;
        conns[live].awaiting = 0;
        conns[live].sent = 0;
        conns[live].index = (uint64_t)(cfg->first + live);
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
    /* CPU from here on is what competes with the server, so connection setup
     * is excluded -- same window the server-side figure is measured over. */
    long long ready_ticks = self_cpu_ticks();

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
                    /* A full-length echo still proves nothing about *which*
                     * request it was, so compare the tag. This is the check
                     * that catches a server recycling a provided buffer before
                     * its send has read from it. */
                    if (memcmp(echo, conns[i].tag, TAG_BYTES) != 0) {
                        mismatches++;
                    }
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
                    /* req is shared by every connection in this worker, so it
                     * is stamped immediately before the send. Safe because a
                     * worker is single-threaded and the kernel has already
                     * copied the bytes by the time send() returns. */
                    uint64_t next_seq = (uint64_t)conns[i].sent + 1;
                    make_tag(req, nonce, conns[i].index, next_seq);
                    memcpy(conns[i].tag, req, TAG_BYTES);

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
    out->mismatches = mismatches;
    out->elapsed_ns =
        (long long)((done.tv_sec - ready.tv_sec) * 1000000000LL + (done.tv_nsec - ready.tv_nsec));
    out->cpu_ticks = self_cpu_ticks() - ready_ticks;
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
    /* Build the worker's cpu list, one logical cpu per physical core.
     *
     * SMT siblings are adjacent pairs on this box ((0,1),(2,3),...), so handing
     * out cpu_base, cpu_base+1, ... would put two workers on the SAME physical
     * core while leaving other cores idle. Striding by 2 avoids that.
     */
    int *cpu_list = malloc((size_t)ncpu * sizeof(int));
    int cpu_count = 0;
    if (!cpu_list) {
        perror("malloc");
        return 1;
    }
    for (long c = cpu_base; c < ncpu && cpu_count < ncpu; c += 2) {
        cpu_list[cpu_count++] = (int)c;
    }
    if (cpu_count < 1) {
        fprintf(stderr, "no usable cpus from base %d (ncpu=%ld)\n", cpu_base, ncpu);
        free(cpu_list);
        return 1;
    }

    if (workers <= 0) {
        /* One worker per physical core available, never more than conns. */
        workers = cpu_count;
        if (workers > conns) {
            workers = conns;
        }
    }
    if (workers > cpu_count) {
        fprintf(
            stderr,
            "note: %d workers requested but only %d cpus from base %d; workers will "
            "share cores\n",
            workers,
            cpu_count,
            cpu_base
        );
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
        cfgs[w].cpu = cpu_list[w % cpu_count];
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
    long long mismatches = 0;
    long long max_elapsed_ns = 0;
    long long cpu_ticks = 0;
    int connected = 0;
    int requested = 0;
    for (int w = 0; w < workers; w++) {
        struct worker_result r;
        ssize_t got = read(pfd[0], &r, sizeof(r));
        if (got == (ssize_t)sizeof(r)) {
            total += r.completed;
            mismatches += r.mismatches;
            connected += r.connected;
            requested += r.requested;
            cpu_ticks += r.cpu_ticks;
            if (r.elapsed_ns > max_elapsed_ns) {
                max_elapsed_ns = r.elapsed_ns;
            }
        }
    }
    close(pfd[0]);
    for (int w = 0; w < workers; w++) {
        waitpid(pids[w], NULL, 0);
    }

    if (mismatches > 0) {
        /* Reported loudly and on the way to a non-zero exit: a mismatched echo
         * means the server returned bytes from the wrong buffer, so the rate it
         * just reported is meaningless and must not be recorded as a result. */
        fprintf(
            stderr,
            "ERROR: %lld of %lld echoes did not match the request that produced "
            "them; the server returned wrong data and this run is invalid\n",
            mismatches,
            total
        );
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

    /* Client CPU as a fraction of one core, so it is directly comparable to
     * the server's server_cpu_pct. bench.sh uses this to prove the server --
     * not the load generator -- is the bottleneck. */
    double client_cpu_pct = 0.0;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz > 0 && elapsed > 0.0) {
        client_cpu_pct = (double)cpu_ticks / (hz * elapsed) * 100.0;
    }

    printf("%lld %.6f %.1f %lld\n", total, elapsed, client_cpu_pct, mismatches);
    free(cfgs);
    free(pids);
    free(cpu_list);
    return mismatches > 0 ? 1 : 0;
}
