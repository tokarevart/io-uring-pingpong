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
| 64 | 1.43% | 2.34% |
| 512 | 0.19% | 1.28% |
| 4096 | 6.51% | 4.05% |

Worst cell is **6.51%** (epoll at 4096), against a 35–51% effect — a 5x margin.
The 4096 point is the noisy one for **both** implementations (epoll 6.51%,
io_uring 4.05%): at that connection count the client sits at 98–100% of its 400%
ceiling, so this point is the one most exposed to contention from the load
generator itself. Absolute throughput on this shared VM drifts a few percent
between full runs, so treat the speedup as the figure and the rps as approximate.

UDP is markedly quieter — worst cell 2.02%, and 0.18–1.38% at 64/512/4096 —
because no connection setup or teardown happens inside the window at all.

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

## A point is only a measurement if the server saturated

Every claim here rests on the server being the bottleneck, so `bench.sh` now
enforces it. A run whose `server_cpu_pct` falls below `MIN_SRV_CPU` (default 95)
is treated as a **failed measurement, not a slow one** — it is retried up to 3
times and, if it still under-saturates, **not recorded at all**. The strace pass
is skipped too, rather than paired with incomplete timing data.

This was not hypothetical. One sweep produced `server_cpu_pct` of **43.3%, 45.0%
and 93.7%** at `uring`/64 while reporting 121k–137k rps, which would have been
recorded as a legitimate slowdown. Re-running that single cell under both builds
gave **99.3% every time** at 137k–146k rps — so it was transient contention on a
shared box, not the frame-pointer flags and not a real effect. The old harness
only ever retried on `completed == 0`, so an unsaturated point sailed straight
into the results.

Two things make this cheap to check: the gate is one `awk` comparison against a
CSV column the harness already records, and both directions are covered by a
negative test (`MIN_SRV_CPU=99.9` rejects every point and leaves a header-only
CSV) plus a positive control.

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
| 64 | 93,912 | **141,557** | 1.51x |
| 512 | 115,083 | **161,759** | 1.41x |
| 4096 | 114,771 | **155,321** | 1.35x |

**io_uring is 1.35x–1.51x faster across the whole sweep.**

---

## Syscalls

`strace -c` attached to the server, 64 connections, 1s window. A single point
suffices because the *per-transaction* rate is stable across connection counts.

**epoll** — 6,509 transactions in 1.0s, 13,301 syscalls
**io_uring** — 108,901 transactions in 1.0s, 2,830 syscalls

| | epoll | io_uring |
|---|---:|---:|
| **syscalls per transaction** | **2.044** | **0.0260** |
| `read`/`write` in hot path | one each per txn | **0** |
| `io_uring_enter` | 0 | ~2,800 |
| total | 13,301 | 2,830 |
| transactions in the same window | 6,509 | **108,901** |

**~79x fewer syscalls per transaction**, and 4.6x fewer in total over a window
long enough to compare rates fairly.

The structural difference: `epoll` issues exactly one `read` and one `write` per
transaction and cannot avoid it. The `io_uring` server issues **zero**
`read`/`write` syscalls in steady state; all data moves through the shared SQ/CQ
rings.

Under `strace`, which penalises syscall entry/exit heavily, `io_uring` served
**17.0x** more transactions than `epoll` in the same wall-clock window.

The strace files are `results/bench_strace.csv` (tcp) and
`results/udp_strace.csv` (udp); each pass cleans up only its own raw reports,
so one transport can never delete the other's evidence.

---

## UDP: same design, much smaller win

The second transport (`results/udp.csv`, run with `PROTO_LIST=udp`). Both sides
are stateless — one socket, no accept/connect/close, nothing per client — so the
comparison isolates the syscall tax and nothing else. 3 runs per point, medians.

| conns | epoll rps | io_uring rps | speedup |
|------:|----------:|-------------:|--------:|
| 64 | 157,780 | **182,582** | 1.16x |
| 512 | 191,564 | **219,317** | 1.14x |
| 4096 | 195,312 | **218,612** | 1.12x |

Server CPU 99.3–100.0% throughout, all 9 points verified with `mismatches=0`.

**Proof the client is not the limiter.** The UDP client is closer to its ceiling
than the TCP one (it also pays two syscalls per datagram, like the epoll server
does), so "server saturated" alone is not sufficient evidence — a saturated
client caps throughput just as effectively. Adding client capacity is the test: if
the client were limiting, more workers would raise throughput. At 512
connections:

| client workers | io_uring rps | epoll rps | uring client CPU | epoll client CPU | server CPU |
|---:|---:|---:|---:|---:|---:|
| 4 | **217,633** | **182,461** | 320% | 303% | 99.3–99.7% |
| 4 (repeat) | 210,361 | 188,494 | 325% | 308% | 99.7% |
| 6 | 179,517 | 167,477 | 281% | 255% | 99.3–99.7% |
| 7 | 186,869 | 167,012 | 265% | 244% | 99.3–100.0% |

Throughput goes **down** by 13–18% when workers are added, and client CPU goes
down with it. That is the signature of a server-bound benchmark, not a
client-bound one: extra runnable processes add scheduler and cache contention on
a box whose server is pinned to one core, and nothing gained is left to gain. The
control row (workers=4 re-run last, after the 6 and 7 runs) rules out monotonic
drift — the 4-worker numbers reproduce within ~3%, which is the same sweep-to-sweep
spread seen elsewhere, while the 6/7 shortfall is 13–18%.

So the UDP numbers are server-limited, and the ceiling identified below is a real
budget rather than an artefact of a capped load generator.

**Syscalls** (`results/udp_strace.csv`, 64 connections, 1s window):

| | epoll | io_uring |
|---|---:|---:|
| transactions in the window | 6,794 | 144,969 |
| total syscalls | 13,693 | 2,932 |
| **syscalls per transaction** | **2.016** | **0.0202** |
| | | **~100x fewer** |

That is a *bigger* syscall reduction than TCP's 78x — epoll still pays exactly
two calls per datagram (`recvfrom` + `sendto`), and io_uring still pays none.
And yet the throughput win is much smaller: **1.10x against TCP's 1.41x**.

### The prediction was wrong, and the profile says why

Before implementing this we predicted the UDP ratio would come out *higher* than
TCP's, on the reasoning that io_uring's advantage is a roughly fixed per-operation
tax: if UDP's socket-side work is lighter, dividing a constant by a smaller
number gives a bigger factor. It does not. Per-transaction CPU, measured the same
way as the TCP profile:

| | epoll | io_uring | saving |
|---|---:|---:|---:|
| **TCP** | 8.75 µs | 6.49 µs | **2.26 µs** |
| **UDP** | 5.52 µs | 4.84 µs | **0.68 µs** |

The absolute saving collapses by 3.3x. The syscall/VFS bucket — the thing
io_uring removes — is 2.56 µs of epoll's TCP time but only 0.89 µs of its UDP
time. The tax is per-operation, but its *size* is set by how much VFS work the
operation does, and `recvfrom`/`sendto` on a datagram socket is a much thinner
path than `read`/`write` on a stream: the profile shows the UDP remainder in
`do_syscall_64` (2.51%), `__internal_syscall_cancel` (1.36%), `fdget` (1.09%),
`move_addr_to_user` (0.82%), `__check_object_size` (1.24%) — entry, fd lookup
and address marshalling, with no stream state machine at all.

io_uring's own cost drops too, 0.35 µs → 0.20 µs, so the denominator shrank as
well; both effects push the same way.

So the correct statement is the opposite of the one we set out to test:
**io_uring's advantage scales with how heavy each operation is.** A synchronous
64-byte stream round trip does enough per-operation work for syscall entry to be
a large share of it. A datagram echo does less, and the same 100x syscall
reduction buys a third of the throughput.

One UDP-specific cost is visible and is charged to io_uring, not to the
transport: `io_msg_copy_hdr` at 0.75% is io_uring copying the userspace
`msghdr`/`iovec` on each `sendmsg`. Using a plain `send` is not possible here —
the reply needs a destination address.

## How the profile was produced

Every CPU attribution below comes from [`profile.sh`](profile.sh), run as
`make prof IMPL=<impl> MODE=<fp|flat|stat>`. That script exists because
this measurement went wrong silently three times before it was pinned down:

* `perf -g none` — perf parses `none` as a **workload name**, reports "Failed to
  collect", and writes nothing. `-g none` is not a valid call-graph setting.
* recording started *before* the client — perf faithfully profiles an idle
  server and produces a small, valid, entirely useless file.
* unprivileged perf — `perf_event_paranoid=2` makes perf silently append `:u`
  to every event and drop all kernel samples, so a "successful" profile can
  describe no kernel time at all. With `kptr_restrict=1` the kernel addresses in
  `/proc/kallsyms` are also zeroed for non-root, so a sampled kernel IP could not
  be named even if one were collected.

So `profile.sh` checks the privilege gates up front and **refuses**, then checks
the artefact afterwards: that kernel frames actually landed, and that stacks
actually unwound (mean depth ≥ 5). Each failure mode above is now a hard error
rather than a plausible-looking number.

| impl / proto | samples | with kernel frames | mean depth | perf.data |
|---|---:|---:|---:|---:|
| uring / tcp | 9,824 | 9,798 (99.7%) | 31.2 | 2.9 MB |
| uring / udp | 9,861 | 9,826 (99.6%) | 26.5 | 2.6 MB |
| epoll / tcp | 9,841 | 9,495 (96.5%) | 24.6 | 2.5 MB |
| epoll / udp | 9,861 | 9,588 (97.2%) | 20.2 | 2.1 MB |

The userspace-only share is itself a result: **0.3% of io_uring's samples are
entirely in userspace, against 3.5% for epoll** on tcp. io_uring's event loop is
essentially invisible in its own profile, which is the same conclusion the flat
profile reached from the other direction.

### Frame pointers: why they are used, and what they did not fix

**The whole project is built with `-fno-omit-frame-pointer
-fno-optimize-sibling-calls`**, and `fp` is the only stack mode. Both flags are
required and neither substitutes — the first establishes the `%rbp` chain, the
second stops tail calls severing it, and gcc leaves sibling calls enabled even
with frame pointers, so asking for only the first yields stacks silently
truncated at tail-call sites.

fp sustains the full 1999 Hz. It was chosen over `.eh_frame` unwinding on two
measured grounds: the same profile taken both ways agrees on the top symbol to
within 0.4pp (`_raw_spin_unlock_irqrestore` 9.61% vs 9.98%), and fp costs ~1.2
MB per 4k samples against ~17.5 MB.

Frame-pointer unwinding does work across the syscall boundary on this kernel —
the WSL2 kernel evidently builds with frame pointers, since stacks reach 55
frames deep and terminate cleanly at `entry_SYSCALL_64_after_hwframe`. That was
worth checking rather than assuming.

What it does **not** buy is more userspace frames, because there are none to
have: at `-O3` these servers inline down to **~11 symbols**, of which the only
real one is `main`. `run_udp_loop`, `recycle_buf` and the rest are inlined into
it. So the binaries carry only 2–3 frame-pointer setups, and a check that counts
them cannot distinguish a correct build from a wrong one — it was tried, and
failed a correct build.

They were made unconditional only after being measured against a build without
them, where the delta across the four impl/transport combinations was −0.8%,
−0.3%, +2.6% and −1.0% — one of them *positive*, which is the point: the spread
is noise, so the cost sits below the measurement floor.

One of those is *positive*, which is the point: the spread is noise, so the cost
is below the measurement floor. Consistent with userspace being ~0.3% of samples
— there is almost nothing for the flags to cost.

### Counters: sleep/wake is measured, not assumed

The profile above argues that sleep/wake is not a factor because `io_cqring_wait`
and `io_uring_submit_and_wait` barely register. `MODE=stat` confirms it with
counters (`results/prof/stat.txt`):

| impl | transactions in 5s | context-switches | per transaction | migrations | page faults |
|---|---:|---:|---:|---:|---:|
| uring | 779,007 | 221 | **0.00028** | 0 | 32 |
| epoll | 573,555 | 161 | **0.00028** | 0 | 69 |

Neither server sleeps per transaction, so `SQPOLL`'s remaining appeal is only
the syscall itself — which the profile already priced at 0.04%. Task-clock was
4.73 s of the 5.00 s window (94.5%; the remainder is teardown), matching
`server_cpu_pct` from an independent measurement path.

## Where the time actually goes (tcp)

The syscall counts above show *that* io_uring wins. A CPU profile shows *where
the remaining time is*, and it changes what is worth trying next.

**Method.** Both servers were profiled identically at 512 connections over an 8s
window pinned the same way as the benchmark, sampled with `perf record -e
cpu-clock` (15.9k samples for io_uring, 15.8k for epoll), produced by
`profile.sh` as described above. This box is WSL2 and
exposes **no virtualised PMU** — `cycles` reports `< not supported>` even as root
— so `cpu-clock` software sampling is the only option. That is a real limitation:
there is no IPC, cache-miss, or branch data here, so this can rank functions but
not explain *why* one is hot. Stacks (`--call-graph fp`, 3,940 samples) were
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

### UDP: bundles were the one real candidate, and they cannot work

`IORING_RECVSEND_BUNDLE` is the only io_uring feature whose premise UDP actually
satisfies — many datagrams queued from many senders, which is precisely where
bundles pay. It is also the feature that was dismissed outright for tcp earlier,
on the grounds that one message in flight per connection means bundles never fire.
So it was worth testing properly rather than assuming. Three findings, all
measured on this kernel rather than read off a patch:

**1. Bundles are rejected with `RECVMSG`.** The prep code refuses the combination
(`if (sr->flags & IORING_RECVSEND_BUNDLE) { if (req->opcode == IORING_OP_RECVMSG) return -EINVAL; }`),
and confirmed empirically: `res = -22`. Since the stateless server must use
`RECVMSG` to learn who to reply to, bundles are unavailable to it.

**2. Even where bundles *are* allowed, they never fire for UDP.** On a *connected*
UDP socket — plain `IORING_OP_RECV`, the opcode bundles accept — with a genuine
backlog, the fill rate is exactly **1.0 datagrams per CQE**:

| datagrams queued | CQEs posted | datagrams/CQE |
|---:|---:|---:|
| 1 | 1 | 1.0 |
| 4 | 4 | 1.0 |
| 16 | 16 | 1.0 |
| strict ping-pong, 20,000 rounds | 20,000 | 1.00 |

This is structural, not a kernel bug: a bundle batches *bytes of one stream read*
across several buffers, but `udp_recvmsg` consumes exactly one datagram per pass,
so there is never more than one datagram's worth to combine. Bundles are a
stream concept and simply do not transfer to a datagram transport. (Axboe also
disabled the UDP bundle cases in liburing's tests for the related reason that GSO
coalesces datagrams into a single packet, so a bundle *send* would emit fewer
datagrams than buffers.)

**3. The opportunity itself is real — nothing can take it.** Instrumenting the
stateless server to histogram recv CQEs per `io_uring_enter` under the actual
512-connection ping-pong workload:

```
recv-cqes-per-drain[0..8] = 0 0 0 0 1 0 0 0 0    >=9: 19999    mean = 65.98
```

So the kernel finds a median of ~66 queued datagrams every time it returns to
the socket. That is a deep queue and a genuine batching opportunity — and after
(1) and (2) there is no available feature that can exploit it.

### Optimisations this ruled out

The profile was used to kill three plausible ideas before spending time on them.

| candidate | premise | why the profile says no |
|---|---|---|
| `IORING_RECVSEND_POLL_FIRST` | skip a wasted initial `recv` that returns `EAGAIN` before arming poll | **`io_arm_poll_handler` and `io_poll_create`: 0.00% of 3,940 stacks.** All `io_recv` samples (9.16%) complete inline from `__io_issue_sqe`, as do 85.96% of `io_send`. Data is already queued when the SQE is issued, so there is no failed attempt to skip — the flag would push requests onto a poll path they currently bypass. Likely a regression. |
| `IOSQE_CQE_SKIP_SUCCESS` + NOP barrier | drop 1 of 2 CQEs per transaction | Ceiling is tiny: `io_fill_cqe_aux` (0.06%) + `io_req_post_cqe` (0.18%) ≈ **0.24%** of total. Best case ~0.24% CPU for a large jump in ownership complexity. |
| `IORING_SETUP_SQPOLL` | remove the remaining `io_uring_enter` syscalls | `io_uring_submit_and_wait` **0.04%**, `io_cqring_wait` **0.01%**. Nothing to remove — completions are already inline. Confirmed by counters: 0.00028 context switches per transaction, so the server is not sleeping either. The SQ poll thread would also compete with the server for the single pinned cpu0. |

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

### The ceiling (udp)

The same bound as tcp: **io_uring's own machinery is 4.19%** of UDP's 4.84 µs
per transaction (`io_submit_sqes` 1.06, `io_msg_copy_hdr` 0.75, `io_issue_sqe`
0.69, `io_recvmsg_multishot` 0.58, `io_sendmsg` 0.57, `io_recvmsg` 0.54). If
that cost vanished entirely, throughput would rise **4.4%** — that is the whole
budget, and the bundle investigation above shows no available feature spends it.

The one remaining lever is `IOSQE_CQE_SKIP_SUCCESS` on the sendmsg, which would
remove one of the two CQEs per datagram. Given that completion posting did not
reach the top of the io_uring core list even once, expect well under 1% — likely
inside the ~2–3% run-to-run spread, i.e. not separately measurable here.

### The ceiling (tcp)

io_uring now spends **0.35 µs** per transaction on its own machinery against a
**4.92 µs** shared socket-path floor. There is no remaining order-of-magnitude
to win on the ring side for this workload; the result is close to structural.
Further gains would have to come from the floor itself — larger payloads to
amortise it, or a different transport — which changes the workload rather than
the event engine.

---

## Honest reading

- The **1.35x–1.51x throughput** margin is specific to 64-byte synchronous
  ping-pong over loopback with single-threaded servers. It does not transfer to
  streaming, large payloads, or real NICs.
- The **~79x syscall-per-transaction** reduction is the structural result. It is
  a property of the design (multishot + provided buffers + batching), not of
  this workload, and it is what would carry over — though not the magnitude of
  any throughput benefit.
- The profile shows **76% of the remaining per-transaction cost is TCP/IP/
  netfilter work that neither implementation controls.** The result is close to
  the practical ceiling for this shape, not an early point on a curve.
- **The margin is a property of the operation, not just of io_uring.** The same
  design on UDP cuts syscalls per transaction by ~100x — *more* than TCP's 78x —
  and buys only 1.14x instead of 1.41x, because `recvfrom`/`sendto` does far less
  kernel work per operation than `read`/`write` does. The bigger the operation,
  the more of it syscall entry represents, and the more there is to win.
- io_uring **loses** on this benchmark if implemented naively with one-at-a-time
  SQEs. The results above require multishot accept/recv, the provided buffer
  ring, correct buffer ownership, and correct CQE batching. See the README for
  the implementation details that make it work — and the correctness section
  above for the check that now guards that ownership rule.