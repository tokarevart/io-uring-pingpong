#!/usr/bin/env python3
"""Aggregate bench results into a comparison table.

Usage: ./report.py [timing.csv] [strace.csv] [label]

Timing and syscall data live in separate files on purpose: strace slows the
server by more than an order of magnitude, so mixing the two into one file
would drag the throughput medians down. Timing defaults to results/bench.csv
and syscalls to results/bench_strace.csv.

Pass an empty strace path to skip the syscall section, and a label to replace
the source path in the header (used by `make bench-quick`, which runs from a
temporary file and persists nothing).

tcp and udp are different workloads whose rates are not comparable, so they get
separate CSVs and separate tables. With no arguments both are rendered, when
the udp data exists.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_TIMING = "results/bench.csv"
DEFAULT_STRACE = "results/bench_strace.csv"
UDP_TIMING = "results/udp.csv"
UDP_STRACE = "results/udp_strace.csv"

COLW = 21


def load(path):
    if not path or not Path(path).exists():
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def render(timing, strace_csv, label):
    rows = load(timing)
    if not rows:
        print(f"no rows in {timing} (skipped)")
        return

    # impl -> conns -> [rps]
    data = defaultdict(lambda: defaultdict(list))
    cpu = defaultdict(dict)
    cli = defaultdict(dict)
    nworkers = 1
    failed = []
    for r in rows:
        try:
            impl, conns = r["impl"], int(r["conns"])
            rps = float(r["rps"])
            if rps <= 0:
                failed.append((impl, conns))  # never plot a bogus zero
                continue
            data[impl][conns].append(rps)
            if r.get("server_cpu_pct") and r["server_cpu_pct"] != "NA":
                cpu[impl][conns] = float(r["server_cpu_pct"])
            if r.get("client_cpu_pct") and r["client_cpu_pct"] != "NA":
                cli[impl][conns] = float(r["client_cpu_pct"])
            if r.get("workers"):
                nworkers = int(r["workers"])
        except (ValueError, KeyError, TypeError):
            continue

    if failed:
        print(f"WARNING: dropped {len(failed)} zero-result row(s) "
              f"(failed measurement, not a real 0 rps): {sorted(set(failed))}")

    conns = sorted({c for i in data for c in data[i]})
    impls = sorted(data)

    counts = {len(v) for i in data for v in data[i].values()}
    if len(counts) == 1:
        n = counts.pop()
        how = f"{n} run{'s' if n != 1 else ''}/point"
    else:
        how = "mixed run counts"

    print(f"source: {label}   ({how})")

    # Every cell holds a single right-aligned number, so the column width is
    # fixed and cannot be broken by a wide value.
    hdr = (f"{'conns':>7} | "
           + " | ".join(f"{i:>{COLW}}" for i in impls)
           + " |   speedup")
    sub = f"{'':>7} | " + " | ".join(f"{'rps':>{COLW}}" for i in impls) + " |"
    print(hdr)
    print(sub)
    print("-" * len(hdr))  # same width as the header by construction

    for c in conns:
        cells, meds = [], {}
        for i in impls:
            v = data[i].get(c, [])
            if not v:
                cells.append("n/a")
                continue
            med = statistics.median(v)
            meds[i] = med
            cells.append(f"{med:,.0f}")

        if len(meds) == 2:
            a, b = impls
            sps = f"{meds[b] / meds[a]:.2f}x" if meds[a] else "n/a"
        else:
            sps = "-"
        print(f"{c:>7} | "
              + " | ".join(f"{cell:>{COLW}}" for cell in cells)
              + f" |   {sps:>6}")

    # Server CPU: proof the server, not the client, was the bottleneck.
    sat = [v for i in cpu for v in cpu[i].values()]
    if sat:
        print(f"\nserver CPU: min {min(sat):.1f}%  max {max(sat):.1f}%  "
              f"(median run {'saturated' if min(sat) >= 95 else 'NOT saturated everywhere'})")

    # The server being pegged shows it is busy; it does NOT prove the load
    # generator is keeping up. A saturated client would cap throughput just as
    # effectively, so report it alongside. Values are summed over worker
    # processes, so 400% with 4 workers means every worker was pegged.
    ccli = [v for i in cli for v in cli[i].values()]
    if ccli:
        # Each worker is pinned to its own core, so n workers can absorb at most
        # n*100% before the load generator itself becomes the limiter.
        ceiling = 100.0 * nworkers
        peak = max(ccli)
        if peak >= ceiling * 0.95:
            # Client is at its ceiling, but that alone does not make it the
            # limiter: if the server is also saturated, adding client workers
            # raises the ceiling without raising throughput, which means the
            # server is what caps the result. Both readings are reported.
            verdict = ("client at ceiling AND server saturated -> server-limited "
                       "(raising WORKERS does not raise throughput)")
        else:
            verdict = "client has headroom -> server-limited"
        print(f"client CPU: {min(ccli):.0f}-{peak:.0f}% summed over {nworkers} worker(s)"
              f" (ceiling {ceiling:.0f}%)")
        print(f"  {verdict}")

    # Syscalls come from the separate strace file; skipped entirely when not asked for.
    if not strace_csv:
        return

    # Keep BOTH the syscall count and the transaction count per (impl, conns).
    #
    # Only the ratio of per-transaction rates is meaningful. strace stops on
    # every syscall, so within a fixed window the implementation that issues the
    # most syscalls per transaction is penalised hardest: epoll (2.04 syscalls
    # per txn) runs ~15x slower under strace, io_uring (0.026) only ~1.3x. That
    # stretches a real 1.48x throughput gap into a 17x transaction-count gap,
    # which measures strace's overhead, not the server.
    counts = defaultdict(dict)
    for r in load(strace_csv):
        try:
            if r.get("syscalls") and r["syscalls"] != "NA":
                completed = int(r.get("completed") or 0)
                counts[r["impl"]][int(r["conns"])] = (int(r["syscalls"]), completed)
        except (ValueError, KeyError, TypeError):
            continue

    if len(counts) == 2:
        a, b = sorted(counts)
        print(f"\nsyscalls per transaction (from {strace_csv}):")
        for c in sorted(set(counts[a]) | set(counts[b])):
            sa, ca = counts[a].get(c, (0, 0))
            sb, cb = counts[b].get(c, (0, 0))
            if not (sa and sb):
                print(f"  conns={c:<6} no syscall data")
                continue
            # `completed` can be 0 (or absent) if the transaction count was not
            # captured, which would make the per-transaction rate undefined.
            pa = f"{sa / ca:.4f}/txn ({sa:,}/{ca:,})" if ca else "n/a"
            pb = f"{sb / cb:.4f}/txn ({sb:,}/{cb:,})" if cb else "n/a"
            ratio = f"{sa / ca / (sb / cb):.1f}x fewer for {b}" if (ca and cb) else "n/a"
            print(f"  conns={c:<6} {a}={pa:>24}   {b}={pb:>24}   {ratio}")
        print("  raw counts come from a strace-instrumented window and are not a")
        print("  throughput comparison; only the per-transaction rate is.")
    elif counts:
        print(f"\nsyscalls recorded for only one impl: {sorted(counts)}")
    else:
        print(f"\nno syscall data at {strace_csv} (run: make strace)")


def main():
    timing = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TIMING
    strace_csv = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_STRACE
    label = sys.argv[3] if len(sys.argv) > 3 else timing

    if not Path(timing).exists():
        sys.exit(f"no timing data at {timing} (run: make bench)")

    render(timing, strace_csv, label)

    # An explicit timing file means the caller wants exactly that one (bench-quick
    # renders a throwaway run from a temp file). Only the default invocation also
    # picks up the udp table.
    if len(sys.argv) <= 1 and Path(UDP_TIMING).exists():
        print()
        render(UDP_TIMING, UDP_STRACE, UDP_TIMING)


main()