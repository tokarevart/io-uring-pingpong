# io_uring vs epoll — a ping-pong echo server benchmark

A C23 benchmark comparing a single-threaded `epoll` echo server against a
single-threaded, `io_uring`-optimised one, under a request/response workload.

The point is **not** that `io_uring` is inherently faster. It is that a naive
`io_uring` server is *slower* than `epoll`, because every operation submitted
one-at-a-time pays ring-tracking overhead per syscall. `io_uring` only wins when
it uses the things `epoll` cannot do: **multishot requests, provided buffer
rings, and batching**.

---

## Quick start

```bash
make                 # build all three binaries into build/
make bench           # tcp + udp timing matrix + syscall passes, then the tables
make bench-udp       # udp only
make report          # re-print the tables from existing CSVs (instant, no re-run)
make strace          # re-run only the syscall-counting passes, then report
```

Everything is tuned to finish **well under a minute end to end**. `make bench`
is the only target that measures anything, and it does both passes: the timing
matrix at 3s per point, then a 1s strace pass, so syscall counts are always in
its output. The strace window is shorter because that pass *counts* syscalls
rather than measuring throughput — epoll still issues ~13k calls in 1s, and the
syscalls-per-transaction ratio is scale-invariant.

**tcp and udp are separate experiments**, each with its own CSV
(`results/bench.csv`, `results/udp.csv`) and its own table. Their rates are not
comparable — a datagram is not a stream — so `report.py` never merges them.

`make report` is the read-only view: it just re-renders
`results/bench.csv` + `results/bench_strace.csv`, so you can look at the
numbers again without re-running anything.

`make bench` **truncates** `results/bench.csv` on every invocation, so it always
reflects one self-contained experiment. Set `APPEND=1` to merge into existing
data instead.

The strace pass writes a **separate** file (`results/bench_strace.csv`) on
purpose: strace slows the server by more than 10x, so sharing a file with the
timing data would drag the reported rates down. The two can never contaminate
each other regardless of the order you run them in.

A point that comes back with zero completed transactions (a port that hasn't
finished releasing, or a connection burst that outruns the accept queue) is
**retried up to 3 times** rather than recorded, and `report.py` drops any
zero-result row with a warning rather than plotting it as `0 rps`.

For tighter bounds, raise `RUNS` — the report then shows the **median** across
repeats and states how many runs went into each point. Keep the total in mind:
this is a latency-sensitive measurement on a shared box, so more repeats means
proportionally longer runs.

Other overrides (all env vars):

```bash
CONNS_LIST="256" ./bench.sh            # single point
IMPLS=uring ./bench.sh                  # one implementation
PROTO_LIST=udp ./bench.sh               # one transport (tcp | udp)
WORKERS=8 ./bench.sh                    # more client worker processes
SKIP_STRACE=1 ./bench.sh                # timing only
make bench-quick CONNS=1024 IMPLS=uring # single point, both impls, ~8s
```

---

## The workload

Deliberately **ping-pong**, not streaming.

If the server just loops `write()` on a firehose, `epoll` + non-blocking
`write()` saturates socket buffers at memory-bandwidth speed and the syscall
overhead is amortised into nothing. Both implementations land within noise of
each other and the comparison says nothing.

Ping-pong instead forces a **synchronous round trip per request**:

1. client sends a fixed-size request (default 64 bytes)
2. server echoes it back verbatim
3. client waits for *that* echo, then sends the next request

Each connection has at most one request in flight. That makes event
notification frequent and syscall overhead visible, which is the thing under
test.

`TCP_NODELAY` is set on both sides so request coalescing doesn't distort the
measurement.

### Counting a "transaction"

A transaction is counted **only when the server's echo `send()` completes
successfully**, not when data arrives. Counting `recv` completions would
overcount whenever TCP splits or coalesces a logical message across
completions. Both servers count the same way, and the client counts
independently (echo fully received), so the two sides can be cross-checked.

### Every echo is content-verified

Counting a full-length echo proves nothing about *which* request it was. Every
request therefore carries a 16-byte tag — a per-run nonce, the connection's
global index, and that request's sequence number — and the client compares the
echo against it.

This guards the `io_uring` server's most delicate invariant. The provided buffer
must be held until the `send` reading from it completes; a buffer recycled early
still yields a full-length echo of the *wrong bytes*, which a length check
cannot detect. Without this the benchmark had no way to catch that failure and
would have reported a clean rps for a corrupting server.

On mismatch the client exits non-zero and `bench.sh` aborts the sweep without
recording the point, so wrong data can never be plotted as a fast result. The
check was validated against a deliberately injected fault; see
[Correctness: every echo is verified](RESULTS.md#correctness-every-echo-is-verified).

---

## The two servers

Both are **single-threaded**. That is deliberate: it isolates the event engine
as the only variable. Multi-threading either side would confound the result
with core-count effects.

### `epoll_srv.c` — the control group

Deliberately tuned to be *competitive*, not a strawman:

- `O_NONBLOCK` everywhere; `accept4(..., SOCK_NONBLOCK)`
- `EPOLLET` with full read draining until `EAGAIN`
- **partial writes are buffered**, never dropped — an unwritten remainder is
  stashed and flushed via `EPOLLOUT`, then `EPOLLIN` is re-armed
- **fd → slot indirection** rather than indexing a fixed-size array by fd
- deadline checked every 1024 iterations, keeping `clock_gettime` out of the
  hot path

### `uring_srv.c` — the optimised version

```c
params.flags = IORING_SETUP_SINGLE_ISSUER
             | IORING_SETUP_DEFER_TASKRUN
             | IORING_SETUP_COOP_TASKRUN;
```

- **one** multishot `accept` SQE for the whole listener — no `epoll_ctl` churn
- **one** multishot `recv` SQE per connection, with `IOSQE_BUFFER_SELECT`
- a **provided buffer ring** (`io_uring_setup_buf_ring`, 2048 × 64 B), so reads
  need no per-connection buffer allocation. Buffer size equals the payload size
  and both are pinned together; see
  [Buffer size is not a lever](RESULTS.md#buffer-size-is-not-a-lever). In UDP
  mode the buffer is `PAYLOAD_SIZE + 32` bytes because of the recvmsg aux
  layout, so the coupling there is slightly different but just as strict.
- batched CQE draining — one `io_uring_enter` per wakeup, then walk every CQE

#### Buffer ownership

This is the subtle part, and getting it wrong is the difference between a
working server and a corrupt one.

A buffer is owned by the kernel from the moment `recv` completes with
`IORING_CQE_F_BUFFER` until the app explicitly returns it with
`io_uring_buf_ring_add` + `io_uring_buf_ring_advance`. So:

- on `recv` completion → queue a `send` from that buffer, **do not** recycle it
  yet (the `send` is still reading from it)
- on `send` completion → **now** recycle the buffer
- on `recv` EOF / error / no-buffer-selected → the buffer is *not* ours; just
  close the fd

Recycling early "works" under low load and corrupts data under load, which is
why it's called out here — and why the client verifies echo *content*, not just
length. That is the only thing that actually detects a violation of this rule.

#### `user_data` packing and fd reuse

`user_data` carries an op tag, a connection generation, the fd, and the buffer
id (bit layout in [Correctness invariants](#correctness-invariants) below).

The generation exists because **fd numbers get recycled**. If connection A on
fd 7 closes and connection B is then accepted as fd 7, a late completion still
tagged with the old generation must not be attributed to B. Every completion
is validated against a per-fd generation counter before acting on it.

#### UDP mode (`proto=udp`)

Datagrams carry no connection, so the UDP path is **stateless on both sides**:
one socket, one permanently-armed multishot `RECVMSG`, and every echo goes back
to whatever `sockaddr` the kernel hands us. There is no accept, no `connect`, no
`close`, no per-client generation counter, and no EOF to detect — which is the
point: with zero per-connection machinery on either side, the comparison
isolates exactly "2 syscalls vs 0 syscalls per datagram" with nothing else
differing. `TCP_NODELAY` is not set either; UDP has no stream to coalesce.

Two things are worth knowing about the io_uring side:

- **Multishot `RECVMSG` cannot return a bare payload.** The kernel prepends the
  aux layout (`struct io_uring_recvmsg_out`, then the source `sockaddr`) *inside*
  the provided buffer, so a UDP buffer is 32 bytes larger than the payload and
  the echo length must come from the aux `payloadlen` field — **`cqe->res` is
  aux+payload together**, not the payload length. A `NULL` msghdr is rejected
  with `-EFAULT`; it is required, because the kernel needs it to lay out the aux.
- **The buffer-ownership rule does not go away.** Each in-flight buffer owns the
  `msghdr`/`iovec` its echo is sent from (indexed by `buf_id`), so the same
  window applies to both, and the client's content check covers this mode too.

A datagram larger than the buffer is truncated silently by `recv`, so `MSG_TRUNC`
and an unexpected `payloadlen` are counted as errors and dropped rather than
echoed as garbage — the same hazard as the `BUF_SIZE`/`MSG_SIZE` coupling
below, guarded the same way.

---

## Correctness invariants

Two things in the `io_uring` server are easy to get wrong and fail *silently* —
no error, no crash, just wrong or absent results. Both are load-bearing.

### `io_uring_cq_advance` takes a **count**, not the head index

`head` from `io_uring_for_each_cqe` is a ring *index*. `cq_advance` expects how
many CQEs you actually consumed. Passing the index makes the head run ahead of
reality, so the kernel refills slots that were never drained and you consume
garbage completions. This is why the server counts explicitly:

```c
unsigned count = 0;
io_uring_for_each_cqe(&ring, head, cqe) {
    count++;
    ...
}
io_uring_cq_advance(&ring, count);
```

### The generation field must not overlap the op tag

The packed `user_data` gives each bit range exactly one meaning:

```
bits 63..62  op    (0 accept, 1 recv, 2 send)   <- 2 bits
bits 61..40  gen   connection generation          <- 22 bits, ends at 61
bits 39..20  fd
bits 19..0   buf_id
```

`gen` stops at bit 61 because bits 62–63 belong to the op tag. If the ranges
overlap, extracting `gen` picks up an op bit, every completion fails the
`fd_gen[fd] != gen` check, and all traffic is discarded as stale — while the
server looks perfectly healthy.

---

## Methodology

The whole point is that **the server must be the bottleneck**. If the client is
limiting, the numbers measure the client, not the event engine.

### CPU pinning

Measured topology on the test box (AMD Ryzen 7 5700X3D, 8 cores / 16 threads,
SMT on, sibling pairs adjacent):

| role | cpus |
|---|---|
| server (single-threaded) | `0` |
| client workers | `2`, `4`, `6`, `8` |

`cpu1` is the **SMT sibling of `cpu0`** and is deliberately excluded from the
client set. Sharing a physical core between a saturated server and the load
generator pollutes both measurements. The sibling map is read at runtime from
`/sys/devices/system/cpu/cpuN/topology/thread_siblings_list`, not hardcoded.

### The client (`pingpong_cli.c`)

- `poll()`, **not** `select()`. `select()` has a hard `FD_SETSIZE` of 1024 on
  this platform, which caps the connection count regardless of how well either
  server scales.
- **4 worker processes**, each owning a disjoint slice of connections and each
  pinning itself via `sched_setaffinity`. Parent aggregates counts over a pipe.
- Workers are pinned to **one logical cpu per physical core** (`cpu_base` with a
  stride of 2). SMT siblings here are adjacent pairs — `(0,1)`, `(2,3)` — so
  handing out consecutive cpus would put two workers on one core.
- Reports its own CPU as a percentage of one core, summed over workers, so the
  harness can confirm the load generator is not itself the bottleneck.
- Non-blocking connect with a `POLLOUT` wait, so N connections are established
  concurrently instead of serialising on handshakes.
- **The measurement window starts only after every connection is up.** Socket
  setup produces zero transactions, so including it would deflate the reported
  rate in proportion to connection count. Workers report their own window and
  the parent uses the longest, and it warns if fewer connections were
  established than requested.

Worker count was tuned empirically (and re-checked for udp, where the client pays
two syscalls per datagram and so sits closer to its ceiling — adding workers still
*lowers* throughput, by 13–18%, which is what a server-bound run looks like). More
is *worse*:

| workers | server CPU | throughput |
|---|---|---|
| 1 | 90% | 735k |
| 2 | 99.4% | 830k |
| 3 | 99.6% | 808k |
| 4 | 99.4% | 810k |
| 8 | 99.4% | 677k |
| 14 | 99.2% | 598k |

2–4 workers fully saturate the server; beyond that, extra runnable processes
just add scheduler and cache contention. Default is **4**.

### Proof the server was saturated

Every run records server CPU utilisation (`utime+stime` from
`/proc/<pid>/stat`) into the CSV as `server_cpu_pct`. Every measured run is
**≥99.0%** — the server is the bottleneck, by measurement rather than assumption.

### Statistics

- 1s warmup, once per implementation (not per point — every run starts a fresh
  server, so a per-point warmup only re-warms global state)
- **3s** measurement window
- **1 run** per point

A 3s window is enough: the effect is ~45% while the worst measured run-to-run
standard deviation is 2.27% — a ~20x margin. Per-cell variation is tabulated in
[`RESULTS.md`](RESULTS.md).

Raise `RUNS` for tighter bounds; the report then shows the **median** across
repeats and states how many runs went into each point (or `mixed run counts` if
they differ).

The table shows one right-aligned number per cell and nothing else. Spread is
not rendered, to keep every cell the same width; the raw `run` column in the
CSV is there when you want to compute it yourself.

An environment snapshot (kernel, CPU, gcc, liburing, and the exact settings
used) is written to `results/env.txt` alongside every run, so a CSV stays
self-describing when compared across machines.

### Result files

| file | contents |
|---|---|
| `results/bench.csv` | `impl,conns,run,completed,elapsed,rps,server_cpu_pct,client_cpu_pct,workers` |
| `results/bench_strace.csv` | `impl,conns,completed,elapsed,syscalls` |
| `results/strace_<impl>_<conns>.txt` | raw `strace -c` reports |
| `results/env.txt` | environment + settings for the run |

The two CSVs carry **different columns on purpose**. The timing file has no
`syscalls` column, and the strace file has no `rps` or `server_cpu_pct`: under
strace the server runs an order of magnitude slower, so its `rps` would be an
artefact of tracing overhead rather than a real rate, and its CPU share is never
measured. Neither file carries a column it always fills with `NA`.

The strace file keeps `completed` because syscalls-per-transaction is derived
from it.

`make bench-quick` runs from a `mktemp` file that is **deleted afterwards**, so
a quick check persists nothing and cannot alter the recorded `bench.csv`. It
prints only its own run, not the full matrix.

`make clean` deliberately **keeps** `results/` — that data is the recorded
evidence. Use `make distclean` to drop it.

---

## Results

Full write-up in [`RESULTS.md`](RESULTS.md); raw data in
[`results/bench.csv`](results/bench.csv).

Re-print the table at any time with `make report` (instant, no re-run).
Regenerate it with `make bench` (~24s; medians of 3 runs with `RUNS=3`).

Headline, on this box:

| | epoll | io_uring |
|---|---:|---:|
| throughput @512 conns, tcp | 110,505 rps | **152,904 rps** (1.38x) |
| throughput @512 conns, udp | 183,043 rps | **205,839 rps** (1.12x) |
| syscalls per transaction, tcp | 2.043 | **0.0260** (~79x fewer) |
| syscalls per transaction, udp | 2.012 | **0.0202** (~100x fewer) |
| `read`/`write` in hot path | one each per txn | **0** |
| echoes failing content check | 0 | **0** |

Server CPU was 99.0–101.0% in every run, and the client had headroom at 64 and
512 connections, so the server was the bottleneck throughout.

**The margin is a property of the operation, not just of io_uring.** UDP cuts
syscalls per transaction by *more* (100x vs 79x) and yet wins by *less* (1.12x vs
1.38x), because syscall entry is a larger share of a stream read/write than of a
datagram `recvfrom`/`sendto`. The heavier the operation, the more there is to
win.

### Where the remaining time goes

A CPU profile of both servers (`perf record -e cpu-clock`; this box is WSL2 and
exposes no PMU, so `cycles` is unavailable) puts io_uring at **6.49 µs per
transaction**, of which:

- **4.92 µs (76%) is TCP + IP + netfilter work that both implementations pay
  identically** and no `io_uring` feature can touch
- **0.35 µs is `io_uring` core** — versus the ~2.3 µs of syscall and VFS entry
  path (`do_syscall_64`, `vfs_read`, `sock_read_iter`, …) it replaces
- **the userspace event loop measures 0.00%**, below the sampling floor

The same profile **rules out three optimisations** before they are attempted:
`IORING_RECVSEND_POLL_FIRST` (`io_arm_poll_handler` at 0.00% of stacks — recv
always completes inline), `CQE_SKIP_SUCCESS` (completion posting is ~0.24% of
CPU), and `SQPOLL` (`io_uring_enter` is 0.04%). Details and the full breakdown
in [Where the time actually goes](RESULTS.md#where-the-time-actually-goes).

---

## Scope and honest caveats

- The margin shown here is specific to this shape: **64-byte synchronous
  ping-pong over loopback, single-threaded servers, `TCP_NODELAY`**. It does
  not transfer to streaming, large payloads, or real NICs.
- **The result is close to structural, not an early point on a curve.** With
  76% of io_uring's per-transaction time in a socket-path floor it shares with
  `epoll`, and only 0.35 µs of its own machinery, there is no further
  order-of-magnitude win available on the ring side for this workload.
- **The UDP mode is not a production-shaped UDP server.** No rate limiting, no
  amplification protection, no idle reaping, and it ignores `MSG_TRUNC`-adjacent
  edge cases beyond counting and dropping them. It exists to isolate the syscall
  tax, nothing more.
- Loopback means the kernel's networking path is exercised but no NIC, no
  DMA, and no real NIC interrupt cost. On this VM, netfilter conntrack, NAT,
  iptables and SELinux account for **13.6%** of server CPU — real cost, but an
  artefact of the environment rather than of either event engine.
- `send_zc` (zero-copy send) is deliberately **not** used — it complicates send
  accounting and adds noise. It would likely widen the gap, but that's a
  separate experiment.
- Syscall counts are the most portable result here: they don't depend on who
  wins the CPU race, and they directly substantiate the syscall-elimination
  claim. The profile's attribution of that same win to the VFS/syscall path is
  corroborating evidence from an independent measurement.
- The io_uring server is single-threaded **and single-ring**. Several workers
  would scale it, but that changes the question being asked.
