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
| Compiler | gcc (SUSE) 14.2, `-std=gnu23 -O3` |
| Topology | loopback (`127.0.0.1`), 64-byte payloads, `TCP_NODELAY` |
| Pinning | server → cpu0; client workers → cpu2,4,6,8 (one logical cpu per physical core; cpu1 excluded as cpu0's SMT sibling) |
| Client | 4 worker processes, `poll()`, one per physical core |
| Protocol | 1s warmup per implementation, 3s measurement window, 3 runs per point (median) |

No `cpfreq` interface is exposed, so the governor cannot be pinned.

### Run-to-run variation

Relative standard deviation across the 3 runs behind each point:

| conns | epoll | io_uring |
|---:|---:|---:|
| 64 | 0.88% | 2.54% |
| 512 | 1.23% | 1.25% |
| 4096 | 2.66% | 8.57% |

Worst cell is **8.57%** (io_uring at 4096), against a ~42–54% effect — still a
5x margin. That cell is also visibly noisier than the rest: its three runs were
162,645 / 149,867 / 136,978 rps, a downward drift rather than random scatter.
At 4096 connections the client sits at 98–100% of its 400% ceiling, so this
point is the one most exposed to contention from the load generator itself.
Absolute throughput on this shared VM drifts a few percent between full runs, so
treat the speedup as the figure and the rps values as approximate.

---

## Correctness: every echo is verified

Every request carries a 16-byte tag — a per-run nonce, the connection's global
index, and that request's sequence number — and the client `memcmp`s the echo
against it. `pingpong_cli` exits non-zero and `bench.sh` aborts the whole sweep
on any mismatch, so a point that served wrong bytes is never recorded as a
result.

This is load-bearing rather than decorative. The buffer-ownership rule in
[`uring_srv.c`](uring_srv.c) — hold the provided buffer until the `send` that
reads from it completes — is exactly the kind of bug a length check cannot see:
a buffer recycled early still produces a full-length echo, just of the wrong
bytes. Before this check the benchmark had no way to detect that class of fault,
which left the project's most delicate invariant untested.

All 18 points in the run below reported `mismatches=0`.

### The check was validated against a deliberate fault

A verification check that has never seen a failure is not known to work. The
server was temporarily modified to flip the first byte of every ~20,000th echo;
the result:

```
!! uring conns=512: 1 echoes had wrong content; run is INVALID and will not be recorded
!! ABORTED: content verification failed; no point from this run was recorded, and the strace pass was skipped
$ echo $?
1
$ cat results/bench.csv      # header only, no data rows
impl,conns,run,completed,elapsed,rps,server_cpu_pct,client_cpu_pct,workers
```

The fault was detected, the CSV was left without rows, and the harness exited
non-zero. The injected code was then reverted; `uring_srv.c` is byte-identical
to its committed state.

---

## Throughput

Server CPU utilisation is recorded per run and is **99.0–101.3% in every run**.

Client CPU is recorded too, summed over workers (ceiling 400% for 4 workers):

| conns | epoll client | io_uring client | % of ceiling |
|---:|---:|---:|---:|
| 64 | 123–125% | 153–156% | 31–39% |
| 512 | 239–243% | 292–308% | 60–77% |
| 4096 | 386–388% | 398–399% | 97–100% |

At 64 and 512 connections the client has clear headroom, so the server is
unambiguously the limiter. At 4096 the client is at its ceiling **and** the
server is saturated; raising `WORKERS` from 4 to 6 leaves throughput unchanged,
which shows the server is what caps the result — a client-limited run would have
gained throughput from the extra workers.

**All figures are therefore server-limited.**

| conns | epoll rps | io_uring rps | speedup |
|------:|----------:|-------------:|--------:|
| 64 | 92,648 | **131,186** | 1.42x |
| 512 | 112,106 | **159,360** | 1.42x |
| 4096 | 97,385 | **149,867** | 1.54x |

**io_uring is 1.42x–1.54x faster across the whole sweep.**

---

## Syscalls

`strace -c` attached to the server, 64 connections, 1s window. A single point
suffices because the *per-transaction* rate is stable across connection counts.

**epoll** — 5,963 transactions in 1.0s, 12,198 syscalls
**io_uring** — 100,054 transactions in 1.0s, 2,603 syscalls

| | epoll | io_uring |
|---|---:|---:|
| **syscalls per transaction** | **2.046** | **0.026** |
| `read`/`write` in hot path | one each per txn | **0** |
| `io_uring_enter` | 0 | ~2,600 |
| total | 12,198 | 2,603 |
| transactions in the same window | 5,963 | **100,054** |

**~79x fewer syscalls per transaction**, and 4.7x fewer in total over a window
long enough to compare rates fairly.

The structural difference: `epoll` issues exactly one `read` and one `write` per
transaction and cannot avoid it. The `io_uring` server issues **zero**
`read`/`write` syscalls in steady state; all data moves through the shared SQ/CQ
rings.

Under `strace`, which penalises syscall entry/exit heavily, `io_uring` served
**16.8x** more transactions than `epoll` in the same wall-clock window.

---

## Where the time actually goes

The syscall counts above show *that* io_uring wins. A CPU profile shows *where
the remaining time is*, and it changes what is worth trying next.

**Method.** Both servers were profiled identically at 512 connections over an 8s
window pinned the same way as the benchmark, sampled with `perf record -e
cpu-clock` (15.9k samples for io_uring, 15.9k for epoll). This box is WSL2 and
exposes **no virtualised PMU** — `cycles` reports `<not supported>` even as root
— so `cpu-clock` software sampling is the only option. That is a real limitation:
there is no IPC, cache-miss, or branch data here, so this can rank functions but
not explain *why* one is hot. Stacks (`--call-graph dwarf`, 3,945 samples) were
captured for attribution. Sampling overhead was negligible: the profiled run
produced 156,995 rps against the benchmark's 159,360.

Per-transaction cost, from the sampled rates:

| | rps | CPU per transaction |
|---|---:|---:|
| io_uring | 156,995 | 6.37 µs |
| epoll | 108,880 | 9.18 µs |
| | | **saving: 2.81 µs** |

Self time grouped by subsystem:

| bucket | io_uring | epoll | delta µs/txn |
|---|---:|---:|---:|
| TCP | 22.49% (1.43) | 16.98% (1.56) | −0.13 |
| IP / net core | 19.36% (1.23) | 15.08% (1.39) | −0.15 |
| locks / RCU / irq / Hyper-V | 17.36% (1.11) | 19.09% (1.75) | −0.65 |
| netfilter / NAT / SELinux | 13.61% (0.87) | 13.99% (1.29) | −0.42 |
| other kernel | 18.51% (1.18) | 28.58% (2.63) | **−1.45** |
| slab / page / memset | 1.89% (0.12) | 1.46% (0.13) | −0.01 |
| timers / sched | 0.97% (0.06) | 1.05% (0.10) | −0.03 |
| memcg accounting | 0.66% (0.04) | 0.79% (0.07) | −0.03 |
| **io_uring core** | **5.01% (0.32)** | — | +0.32 |
| **epoll core** | — | **2.98% (0.27)** | −0.27 |

**Three things fall out of this.**

**1. Three quarters of the remaining cost is a floor both implementations pay
identically.** TCP + IP + netfilter + locks/RCU/irq + slab + timers + memcg is
**4.86 µs of io_uring's 6.37 µs (76%)**, and 6.29 µs of epoll's 9.18 µs (68%).
No `io_uring` feature touches this. `tcp_ack` alone is the largest single leaf
at 3.35%, mostly `tcp_send_delayed_ack` → `sk_reset_timer` → `__mod_timer` —
timer rearming inherent to TCP on loopback, which `TCP_NODELAY` does not
suppress since that governs the sending side.

**2. The entire 2.81 µs win is the "other kernel" bucket, and it is exactly the
syscall tax the design predicts.** For epoll that bucket is dominated by
`do_syscall_64` (3.01%), `__internal_syscall_cancel` (1.18%), `vfs_read`
(1.01%), `sock_read_iter` (0.90%), `fdget_pos` (0.74%), `sock_poll` (0.59%),
`sock_write_iter` + `vfs_write` (1.13%) — the VFS/syscall entry path that
`io_uring` replaces with **0.32 µs** of `io_uring core`. The ~79x syscall
reduction from the previous section and this 1.45 µs are the same fact measured
two ways: once in the kernel's syscall counter, once in the kernel's own profile.

**3. The userspace event loop is invisible.** **0.00%** of samples — below the
sampling floor at 3,945 samples (0.025%), with neither `uring_srv` nor `main`
appearing as a leaf. Per-CQE decode, SQE prep, and buffer recycling cost
nothing measurable. **Any optimisation aimed at the userspace loop cannot pay.**

### Optimisations this ruled out

The profile was used to kill three plausible ideas before spending time on them.

| candidate | premise | why the profile says no |
|---|---|---|
| `IORING_RECVSEND_POLL_FIRST` | skip a wasted initial `recv` that returns `EAGAIN` before arming poll | **`io_arm_poll_handler` and `io_poll_create`: 0.00% of 3,945 stacks.** All 406 `io_recv` and 3,337 `io_send` samples complete inline from `__io_issue_sqe`. Data is already queued when the SQE is issued, so there is no failed attempt to skip — the flag would push requests onto a poll path they currently bypass. Likely a regression. |
| `IOSQE_CQE_SKIP_SUCCESS` + NOP barrier | drop 1 of 2 CQEs per transaction | Ceiling is tiny: `io_fill_cqe*` + `__io_submit_flush_completions` ≈ **0.31%** of total. Best case ~0.3% CPU for a large jump in ownership complexity. |
| `IORING_SETUP_SQPOLL` | remove the remaining `io_uring_enter` syscalls | `__do_sys_io_uring_enter` **0.04%**, `io_cqring_wait` **0.03%**. Nothing to remove — completions are already inline. The SQ poll thread would also compete with the server for the single pinned cpu0. |

`accept_direct` + `close_direct` remain available and correct, but they save one
syscall per *connection*, not per transaction: not a throughput lever, only a
tightening of the "zero syscalls in steady state" claim.

The CQ is already **8,192 entries** at `ENTRIES=4096` (the kernel defaults CQ to
2× SQ), so `IORING_SETUP_CQSIZE` at 8,192 would be a no-op. Raising it further is
insurance against overflow at high connection counts, not a measured gain.

### The ceiling

io_uring now spends **0.32 µs** per transaction on its own machinery against a
**4.86 µs** shared socket-path floor. There is no remaining order-of-magnitude
to win on the ring side for this workload; the result is close to structural.
Further gains would have to come from the floor itself — larger payloads to
amortise it, or a different transport — which changes the workload rather than
the event engine.

---

## Honest reading

- The **1.42x–1.54x throughput** margin is specific to 64-byte synchronous
  ping-pong over loopback with single-threaded servers. It does not transfer to
  streaming, large payloads, or real NICs.
- The **~79x syscall-per-transaction** reduction is the structural result. It is
  a property of the design (multishot + provided buffers + batching), not of
  this workload, and it is what would carry over — though not the magnitude of
  any throughput benefit.
- The profile shows **76% of the remaining per-transaction cost is TCP/IP/
  netfilter work that neither implementation controls.** The 1.4–1.5x is close
  to the practical ceiling for this shape, not an early point on a curve.
- io_uring **loses** on this benchmark if implemented naively with one-at-a-time
  SQEs. The results above require multishot accept/recv, the provided buffer
  ring, correct buffer ownership, and correct CQE batching. See the README for
  the implementation details that make it work — and the correctness section
  above for the check that now guards that ownership rule.