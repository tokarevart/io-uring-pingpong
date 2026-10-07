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
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

TIMING = sys.argv[1] if len(sys.argv) > 1 else "results/bench.csv"
STRACE = sys.argv[2] if len(sys.argv) > 2 else "results/bench_strace.csv"
LABEL = sys.argv[3] if len(sys.argv) > 3 else TIMING

COLW = 21

if not Path(TIMING).exists():
    sys.exit(f"no timing data at {TIMING} (run: make bench)")


def load(path):
    if not path or not Path(path).exists():
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


rows = load(TIMING)
if not rows:
    sys.exit(f"no rows in {TIMING} (run: make bench)")

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

print(f"source: {LABEL}   ({how})")

# Every cell holds a single right-aligned number, so the column width is fixed
# and cannot be broken by a wide value.
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
if not STRACE:
    sys.exit(0)

syscalls = defaultdict(dict)
for r in load(STRACE):
    try:
        if r.get("syscalls") and r["syscalls"] != "NA":
            syscalls[r["impl"]][int(r["conns"])] = int(r["syscalls"])
    except (ValueError, KeyError, TypeError):
        continue

if len(syscalls) == 2:
    a, b = sorted(syscalls)
    print(f"\nsyscalls per point (from {STRACE}):")
    for c in sorted(set(syscalls[a]) | set(syscalls[b])):
        va, vb = syscalls[a].get(c), syscalls[b].get(c)
        if va and vb:
            print(f"  conns={c:<6} {a}={va:>8,}   {b}={vb:>8,}   {va / vb:.1f}x fewer for {b}")
        else:
            print(f"  conns={c:<6} {a}={va or 'n/a':>8}   {b}={vb or 'n/a':>8}")
elif syscalls:
    print(f"\nsyscalls recorded for only one impl: {sorted(syscalls)}")
else:
    print(f"\nno syscall data at {STRACE} (run: make strace)")
