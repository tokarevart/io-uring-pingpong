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
make bench           # timing matrix + syscall pass, then the table (~23s total)
make report          # re-print the table from existing CSVs (instant, no re-run)
make strace          # re-run only the syscall-counting pass, then report
```

Everything is tuned to finish **well under 30 seconds end to end**. `make bench`
is the only target that measures anything, and it does both passes: the timing
matrix at 3s per point, then a 1s strace pass, so syscall counts are always in
its output. The strace window is shorter because that pass *counts* syscalls
rather than measuring throughput — epoll still issues ~13k calls in 1s, and the
syscalls-per-transaction ratio is scale-invariant.

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
- a **provided buffer ring** (`io_uring_setup_buf_ring`, 2048 × 512 B), so reads
  need no per-connection buffer allocation
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
why it's called out here.

#### `user_data` packing and fd reuse

`user_data` carries an op tag, a connection generation, the fd, and the buffer
id (bit layout in [Correctness invariants](#correctness-invariants) below).

The generation exists because **fd numbers get recycled**. If connection A on
fd 7 closes and connection B is then accepted as fd 7, a late completion still
tagged with the old generation must not be attributed to B. Every completion
is validated against a per-fd generation counter before acting on it.

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
| client workers | `2`–`15` |

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
- Non-blocking connect with a `POLLOUT` wait, so N connections are established
  concurrently instead of serialising on handshakes.
- **The measurement window starts only after every connection is up.** Socket
  setup produces zero transactions, so including it would deflate the reported
  rate in proportion to connection count. Workers report their own window and
  the parent uses the longest, and it warns if fewer connections were
  established than requested.

Worker count was tuned empirically. More is *worse*:

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

A 3s window is enough: the effect is ~45% while measured run-to-run standard
deviation is 1.6% (epoll) and 3.4% (io_uring) — an order of magnitude below the
effect size.

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
| `results/bench.csv` | `impl,conns,run,completed,elapsed,rps,server_cpu_pct` |
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
| throughput @512 conns | 107,522 rps | **147,091 rps** (1.37x) |
| syscalls per transaction | 2.043 | **0.0260** (~79x fewer) |
| `read`/`write` in hot path | 12,587 | **0** |

Server CPU was 99.0–101.0% in every run, so the server was the bottleneck
throughout.

---

## Scope and honest caveats

- The margin shown here is specific to this shape: **64-byte synchronous
  ping-pong over loopback, single-threaded servers, `TCP_NODELAY`**. It does
  not transfer to streaming, large payloads, or real NICs.
- Loopback means the kernel's networking path is exercised but no NIC, no
  DMA, and no real NIC interrupt cost.
- `send_zc` (zero-copy send) is deliberately **not** used — it complicates send
  accounting and adds noise. It would likely widen the gap, but that's a
  separate experiment.
- Syscall counts are the most portable result here: they don't depend on who
  wins the CPU race, and they directly substantiate the syscall-elimination
  claim.
