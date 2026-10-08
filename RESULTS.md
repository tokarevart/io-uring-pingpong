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
| 64 | 1.72% | 0.45% |
| 512 | 0.55% | 1.50% |
| 4096 | 4.66% | 4.42% |

Worst cell is **4.66%** (epoll at 4096), against a 38–54% effect — a 10x margin.
The 4096 point is visibly the noisy one for **both** implementations (epoll
104,666–114,898, uring 146,689–160,111): at that connection count the client
sits at 98–100% of its 400% ceiling, so this point is the one most exposed to
contention from the load generator itself. Absolute throughput on this shared VM
drifts a few percent between full runs, so treat the speedup as the figure and
the rps values as approximate.

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
| 64 | 123–125% | 158–159% | 31–40% |
| 512 | 244–245% | 298–313% | 61–78% |
| 4096 | 391–396% | 397–399% | 98–100% |

At 64 and 512 connections the client has clear headroom, so the server is
unambiguously the limiter. At 4096 the client is at its ceiling **and** the
server is saturated; raising `WORKERS` from 4 to 6 leaves throughput unchanged,
which shows the server is what caps the result — a client-limited run would have
gained throughput from the extra workers.

**All figures are therefore server-limited.**

| conns | epoll rps | io_uring rps | speedup |
|------:|----------:|-------------:|--------:|
| 64 | 94,222 | **144,766** | 1.54x |
| 512 | 114,933 | **161,647** | 1.41x |
| 4096 | 110,342 | **152,056** | 1.38x |

**io_uring is 1.38x–1.54x faster across the whole sweep.**

---

## Syscalls

`strace -c` attached to the server, 64 connections, 1s window. A single point
suffices because the *per-transaction* rate is stable across connection counts.

**epoll** — 6,380 transactions in 1.0s, 13,037 syscalls
**io_uring** — 108,560 transactions in 1.0s, 2,826 syscalls

| | epoll | io_uring |
|---|---:|---:|
| **syscalls per transaction** | **2.043** | **0.0260** |
| `read`/`write` in hot path | one each per txn | **0** |
| `io_uring_enter` | 0 | ~2,800 |
| total | 13,037 | 2,826 |
| transactions in the same window | 6,380 | **108,560** |

**~79x fewer syscalls per transaction**, and 4.6x fewer in total over a window
long enough to compare rates fairly.

The structural difference: `epoll` issues exactly one `read` and one `write` per
transaction and cannot avoid it. The `io_uring` server issues **zero**
`read`/`write` syscalls in steady state; all data moves through the shared SQ/CQ
rings.

Under `strace`, which penalises syscall entry/exit heavily, `io_uring` served
**17.0x** more transactions than `epoll` in the same wall-clock window.

---

## Where the time actually goes

The syscall counts above show *that* io_uring wins. A CPU profile shows *where
the remaining time is*, and it changes what is worth trying next.

**Method.** Both servers were profiled identically at 512 connections over an 8s
window pinned the same way as the benchmark, sampled with `perf record -e
cpu-clock` (15.9k samples for io_uring, 15.8k for epoll). This box is WSL2 and
exposes **no virtualised PMU** — `cycles` reports `< not supported>` even as root
— so `cpu-clock` software sampling is the only option. That is a real limitation:
there is no IPC, cache-miss, or branch data here, so this can rank functions but
not explain *why* one is hot. Stacks (`--call-graph dwarf`, 3,940 samples) were
captured for attribution. Sampling overhead was negligible: the profiled run
produced 154,091 rps against the benchmark's 161,647.

Per-transaction cost, from the sampled rates:

| | rps | CPU per transaction |
|---|---:|---:|
| io_uring | 154,091 | 6.49 µs |
| epoll | 114,266 | 8.75 µs |
| | | **saving: 2.26 µs** |

Self time grouped by subsystem:

| bucket | io_uring | epoll | delta µs/txn |
|---|---:|---:|---:|
| TCP | 22.29% (1.45) | 17.86% (1.56) | −0.12 |
| IP / net core | 18.76% (1.22) | 15.65% (1.37) | −0.15 |
| locks / RCU / irq / Hyper-V | 17.71% (1.15) | 16.21% (1.42) | −0.27 |
| netfilter / NAT / SELinux | 13.61% (0.88) | 14.80% (1.30) | −0.41 |
| other kernel | 18.58% (1.21) | 29.21% (2.56) | **−1.35** |
| slab / page / memset | 1.85% (0.12) | 1.47% (0.13) | −0.01 |
| timers / sched | 0.93% (0.06) | 1.01% (0.09) | −0.03 |
| memcg accounting | 0.69% (0.04) | 0.82% (0.07) | −0.03 |
| **io_uring core** | **5.46% (0.35)** | — | +0.35 |
| **epoll core** | — | **2.84% (0.25)** | −0.25 |

**Three things fall out of this.**

**1. Three quarters of the remaining cost is a floor both implementations pay
identically.** TCP + IP + netfilter + locks/RCU/irq + slab + timers + memcg is
**4.92 µs of io_uring's 6.49 µs (76%)**, and 5.94 µs of epoll's 8.75 µs (68%).
No `io_uring` feature touches this. `tcp_ack` alone is the largest single leaf
at 3.22%, mostly `tcp_send_delayed_ack` → `sk_reset_timer` → `__mod_timer` —
timer rearming inherent to TCP on loopback, which `TCP_NODELAY` does not
suppress since that governs the sending side.

**2. The entire 2.26 µs win is the "other kernel" bucket, and it is exactly the
syscall tax the design predicts.** For epoll that bucket is dominated by
`do_syscall_64` (2.97%), `__internal_syscall_cancel` (1.29%), `vfs_read`
(1.01%), `sock_read_iter` (0.97%), `fdget_pos` (0.81%), `sock_poll` (0.59%),
`sock_write_iter` + `vfs_write` (1.26%), `__file_has_perm` (0.46%) — the VFS and
syscall-entry path that `io_uring` replaces with **0.35 µs** of `io_uring core`.
The ~79x syscall reduction from the previous section and this 1.35 µs are the
same fact measured two ways: once in the kernel's syscall counter, once in the
kernel's own profile.

**3. The userspace event loop is invisible.** **0.00%** of samples — below the
sampling floor at 3,940 samples (0.025%), with neither `uring_srv` nor `main`
appearing as a leaf. Per-CQE decode, SQE prep, and buffer recycling cost
nothing measurable. **Any optimisation aimed at the userspace loop cannot pay.**

### Optimisations this ruled out

The profile was used to kill three plausible ideas before spending time on them.

| candidate | premise | why the profile says no |
|---|---|---|
| `IORING_RECVSEND_POLL_FIRST` | skip a wasted initial `recv` that returns `EAGAIN` before arming poll | **`io_arm_poll_handler` and `io_poll_create`: 0.00% of 3,940 stacks.** All `io_recv` samples (9.16%) complete inline from `__io_issue_sqe`, as do 85.96% of `io_send`. Data is already queued when the SQE is issued, so there is no failed attempt to skip — the flag would push requests onto a poll path they currently bypass. Likely a regression. |
| `IOSQE_CQE_SKIP_SUCCESS` + NOP barrier | drop 1 of 2 CQEs per transaction | Ceiling is tiny: `io_fill_cqe_aux` (0.06%) + `io_req_post_cqe` (0.18%) ≈ **0.24%** of total. Best case ~0.24% CPU for a large jump in ownership complexity. |
| `IORING_SETUP_SQPOLL` | remove the remaining `io_uring_enter` syscalls | `io_uring_submit_and_wait` **0.04%**, `io_cqring_wait` **0.01%**. Nothing to remove — completions are already inline. The SQ poll thread would also compete with the server for the single pinned cpu0. |

`accept_direct` + `close_direct` remain available and correct, but they save one
syscall per *connection*, not per transaction: not a throughput lever, only a
tightening of the "zero syscalls in steady state" claim.

The CQ is already **8,192 entries** at `ENTRIES=4096` (the kernel defaults CQ to
2× SQ, confirmed in the `io_uring_setup` trace), so `IORING_SETUP_CQSIZE` at
8,192 would be a no-op. Raising it further is insurance against overflow at high
connection counts, not a measured gain.

### Buffer size is not a lever

`BUF_SIZE` was 512 bytes with a 64-byte payload — 8x more buffer than any
request needs. It is now 64, matching the payload exactly. The change is a
clarity fix, not a performance one, and the profile is why: at 64B vs 512B the
only candidate costs are the copy (`_copy_from_iter` 0.66%, `_copy_to_iter`
0.57% — a 64B copy either way, since size-independent) and cache footprint.
A 64B recv writes the first 64B of its buffer and leaves the rest untouched in
both cases, so dirty lines and faulted pages are identical. Measured across both
sizes, the per-bucket breakdown above is unchanged within noise.

What 64B *does* fix is a configuration hazard. The multishot recv is prepared
with `len=0` ("no limit"), so the provided buffer's `io_len` is what actually
caps the transfer — a payload larger than `BUF_SIZE` arrives as several short
echoes. That is loud rather than silent (measured at roughly half the echoes
mismatching, aborting the run), but it presents as content corruption rather
than as the sizing mistake it is. Both sizes are now pinned together:
`BUF_SIZE` in `uring_srv.c` and `MSG_SIZE` in `bench.sh`.

### The ceiling

io_uring now spends **0.35 µs** per transaction on its own machinery against a
**4.92 µs** shared socket-path floor. There is no remaining order-of-magnitude
to win on the ring side for this workload; the result is close to structural.
Further gains would have to come from the floor itself — larger payloads to
amortise it, or a different transport — which changes the workload rather than
the event engine.

---

## Honest reading

- The **1.38x–1.54x throughput** margin is specific to 64-byte synchronous
  ping-pong over loopback with single-threaded servers. It does not transfer to
  streaming, large payloads, or real NICs.
- The **~79x syscall-per-transaction** reduction is the structural result. It is
  a property of the design (multishot + provided buffers + batching), not of
  this workload, and it is what would carry over — though not the magnitude of
  any throughput benefit.
- The profile shows **76% of the remaining per-transaction cost is TCP/IP/
  netfilter work that neither implementation controls.** The result is close to
  the practical ceiling for this shape, not an early point on a curve.
- io_uring **loses** on this benchmark if implemented naively with one-at-a-time
  SQEs. The results above require multishot accept/recv, the provided buffer
  ring, correct buffer ownership, and correct CQE batching. See the README for
  the implementation details that make it work — and the correctness section
  above for the check that now guards that ownership rule.