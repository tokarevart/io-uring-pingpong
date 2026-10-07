# Results

All numbers below come from the recorded data in this repo:
[`results/bench.csv`](results/bench.csv) (timing) and
[`results/bench_strace.csv`](results/bench_strace.csv) (syscalls).
Environment as captured in [`results/env.txt`](results/env.txt).

Re-print this table at any time with `make report` (instant, reads the CSVs only).
Regenerate it with `make bench` (~24s end to end); `RUNS=3` (~60s) produces the
medians recorded here.

## Environment

| | |
|---|---|
| CPU | AMD Ryzen 7 5700X3D — 8 cores / 16 threads (SMT on, siblings adjacent) |
| Kernel | Linux 6.18.40.1-microsoft-standard-WSL2 |
| liburing | 2.11 (`liburing.so.2`) |
| Compiler | gcc (SUSE) 14.2.1, `-std=gnu23 -O3` |
| Topology | loopback (`127.0.0.1`), 64-byte payloads, `TCP_NODELAY` |
| Pinning | server → cpu0; client workers → cpu2,4,6,8 (one logical cpu per physical core; cpu1 excluded as cpu0's SMT sibling) |
| Client | 4 worker processes, `poll()`, one per physical core |
| Protocol | 1s warmup per implementation, 3s measurement window, 3 runs per point (median) |

No `cpufreq` interface is exposed, so the governor cannot be pinned.

### Run-to-run variation

Relative standard deviation across the 3 runs behind each point:

| conns | epoll | io_uring |
|---:|---:|---:|
| 64 | 1.42% | 2.27% |
| 512 | 2.27% | 1.40% |
| 4096 | 1.74% | 0.87% |

Worst cell is 2.27%, against a ~45% effect — a ~20x margin. Absolute
throughput on this shared VM still drifts a few percent between full runs, so
treat the speedup as the figure and the rps values as approximate.

---

## Throughput

Server CPU utilisation is recorded per run and is **99.3–101.3% in every run**.

Client CPU is recorded too, summed over workers (ceiling 400% for 4 workers):

| conns | epoll client | io_uring client | % of ceiling |
|---:|---:|---:|---:|
| 64 | 123% | 152% | 30–38% |
| 512 | 240% | 299% | 60–75% |
| 4096 | 386% | 398% | 96–100% |

At 64 and 512 connections the client has clear headroom, so the server is
unambiguously the limiter. At 4096 the client is at its ceiling **and** the
server is saturated; raising `WORKERS` from 4 to 6 leaves throughput unchanged
(138,528 → 139,048 rps), which shows the server is what caps the result — a
client-limited run would have gained throughput from the extra workers.

**All figures are therefore server-limited.**

| conns | epoll rps | io_uring rps | speedup |
|------:|----------:|-------------:|--------:|
| 64 | 93,703 | **127,020** | 1.36x |
| 512 | 106,043 | **152,482** | 1.44x |
| 4096 | 96,375 | **145,152** | 1.51x |

**io_uring is 1.36x–1.51x faster across the whole sweep.**

Both servers peak at 512 connections and ease off slightly at 4096; neither
degrades as connections are added.

---

## Syscalls

`strace -c` attached to the server, 64 connections, 1s window. A single point
suffices because the *per-transaction* rate is stable across connection counts:
epoll measures 2.045/txn at 64 conns and 2.142 at 256, and io_uring 0.0260
against 0.0279 — both within 7%.

**epoll**

```
  write           6346
  read            6360
  accept4         65
  epoll_wait      2
  total           12927
```

**io_uring**

```
  io_uring_enter  2697
  total           2810
```

| | epoll | io_uring |
|---|---:|---:|
| transactions served | 6,335 | 107,767 |
| total syscalls | 12,927 | 2,810 |
| **syscalls per transaction** | **2.041** | **0.0261** |
| `read`/`write` in hot path | 12,706 | **0** |
| `io_uring_enter` | 0 | 2,697 |

**4.6x fewer syscalls total, ~78x fewer per transaction.**

The structural difference: `epoll` issues exactly one `read` and one `write`
per transaction and cannot avoid it — that is the 2.041. The `io_uring` server
issues **zero** `read`/`write` syscalls in steady state; the handful of `read`
calls in its report are one-time startup. All data moves through the shared
SQ/CQ rings, paid for by 2,697 `io_uring_enter` calls covering 107,767
transactions — **40 transactions per `io_uring_enter`**, the batching payoff.

`epoll_wait` was called only **2 times**: edge-triggered `epoll` with full
draining batches well and is not a strawman. `io_uring` still wins by removing
the per-operation `read`/`write` entirely rather than amortising the wait.

Under `strace` — which penalises syscall entry/exit heavily — `io_uring` served
**17.0x** more transactions than `epoll` in the same wall-clock window, a direct
measure of how much of `epoll`'s cost *is* syscall overhead.

---

## Honest reading

- The **~1.4–1.5x throughput** margin is specific to 64-byte synchronous ping-pong
  over loopback with single-threaded servers. It does not transfer to
  streaming, large payloads, or real NICs.
- The **~78x syscall-per-transaction** reduction is the structural result. It is
  a property of the design (multishot + provided buffers + batching), not of this
  workload, and it is what would carry over — though the *magnitude* of any
  throughput benefit would not.
- `io_uring` **loses** on this benchmark if implemented naively with
  one-at-a-time SQEs. The results above require multishot accept/recv, the
  provided buffer ring, correct buffer ownership, and correct CQE batching. See
  the README for the implementation details that make it work.
