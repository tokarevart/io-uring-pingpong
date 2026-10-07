# Results

All numbers below come from the recorded data in this repo:
[`results/bench.csv`](results/bench.csv) (timing) and
[`results/bench_strace.csv`](results/bench_strace.csv) (syscalls).
Environment as captured in [`results/env.txt`](results/env.txt).

Re-print this table at any time with `make report` (instant, reads the CSVs only).
Regenerate it with `make bench` (~23s end to end).

## Environment

| | |
|---|---|
| CPU | AMD Ryzen 7 5700X3D — 8 cores / 16 threads (SMT on, siblings adjacent) |
| Kernel | Linux 6.18.40.1-microsoft-standard-WSL2 |
| liburing | 2.11 (`liburing.so.2`) |
| Compiler | gcc (SUSE) 14.2.1, `-std=gnu23 -O3` |
| Topology | loopback (`127.0.0.1`), 64-byte payloads, `TCP_NODELAY` |
| Pinning | server → cpu0; client workers → cpu2–cpu15 (cpu1 excluded as SMT sibling of cpu0) |
| Client | 4 worker processes, `poll()` |
| Protocol | 1s warmup per implementation, 3s measurement window, 3 runs per point (median) |

No `cpufreq` interface is exposed, so the governor cannot be pinned.

### Run-to-run variation

Repeat measurements give a relative standard deviation of **1.6% for epoll** and
**3.4% for io_uring** — an order of magnitude below the ~45% effect, so the
comparison is not sensitive to which run you look at. Numbers below are medians
of 3 runs; `make bench` defaults to 1 run per point (~24s), and `RUNS=3` is what
produced this table.

---

## Throughput

Server CPU utilisation is recorded per run and is **99.0–101.0% in every
run** — the server, not the client, was the bottleneck.

| conns | epoll rps | io_uring rps | speedup |
|------:|----------:|-------------:|--------:|
| 64 | 87,917 | **127,994** | 1.46x |
| 512 | 107,522 | **147,091** | 1.37x |
| 4096 | 95,825 | **137,667** | 1.44x |

**io_uring is 1.37x–1.46x faster across the whole sweep.**

Both servers peak at 512 connections and ease off slightly at 4096; neither
degrades as connections are added.

---

## Syscalls

`strace -c` attached to the server, 64 connections, 1s window. A single point
suffices: syscall counts are flat across connection counts (epoll measured
66,372 at 64 conns and 64,394 at 256 — within 3%).

**epoll**

```
  write           6285
  read            6302
  accept4         65
  epoll_wait      2
  total           12814
```

**io_uring**

```
  io_uring_enter  2605
  total           2710
```

| | epoll | io_uring |
|---|---:|---:|
| transactions served | 6,271 | 104,123 |
| total syscalls | 12,814 | 2,710 |
| **syscalls per transaction** | **2.043** | **0.0260** |
| `read`/`write` in hot path | 12,587 | **0** |
| `io_uring_enter` | 0 | 2,605 |

**4.7x fewer syscalls total, ~79x fewer per transaction.**

The structural difference: `epoll` issues exactly one `read` and one `write`
per transaction and cannot avoid it — that is the 2.043. The `io_uring` server
issues **zero** `read`/`write` syscalls in steady state; the handful of `read`
calls in its report are one-time startup. All data moves through the shared
SQ/CQ rings, paid for by 2,605 `io_uring_enter` calls covering 104,123
transactions — **40 transactions per `io_uring_enter`**, the batching payoff.

`epoll_wait` was called only **2 times**: edge-triggered `epoll` with full
draining batches well and is not a strawman. `io_uring` still wins by removing
the per-operation `read`/`write` entirely rather than amortising the wait.

Under `strace` — which penalises syscall entry/exit heavily — `io_uring` served
**16.6x** more transactions than `epoll` in the same wall-clock window, a direct
measure of how much of `epoll`'s cost *is* syscall overhead.

---

## Honest reading

- The **~1.4x throughput** margin is specific to 64-byte synchronous ping-pong
  over loopback with single-threaded servers. It does not transfer to
  streaming, large payloads, or real NICs.
- The **~79x syscall-per-transaction** reduction is the structural result. It is
  a property of the design (multishot + provided buffers + batching), not of this
  workload, and it is what would carry over — though the *magnitude* of any
  throughput benefit would not.
- `io_uring` **loses** on this benchmark if implemented naively with
  one-at-a-time SQEs. The results above require multishot accept/recv, the
  provided buffer ring, correct buffer ownership, and correct CQE batching. See
  the README for the implementation details that make it work.
