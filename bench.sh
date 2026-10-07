#!/bin/bash
# Benchmark harness for io_uring vs epoll.
#
# Methodology notes:
#   - server pinned to one logical cpu; client workers pinned to the rest
#   - the server's SMT sibling is deliberately excluded from the client set,
#     otherwise the two sides share a physical core and pollute each other
#   - default is 1 run x 3s per point. Measured run-to-run stdev is 1.5-2.4%
#     (4.7% worst case) against a ~40% effect, so one run resolves the result.
#   - the strace pass writes results/bench_strace.csv, never the timing CSV, so
#     the two experiments can never contaminate each other
set -uo pipefail

# Ctrl-C must actually abort. Setting a trap on INT/TERM makes bash run the
# handler and *continue*, so without an explicit exit a signal would be
# swallowed and the benchmark would run to completion anyway. These handlers
# kill the in-flight server/strace and exit with the conventional 128+signo.
CURRENT_SRV_PID=""
CURRENT_SRV_BG_PID=""
CURRENT_STRACE_PID=""

cleanup() {
    # No `wait` here: it can block on a process that ignores the signal, which
    # is exactly what makes an interrupt feel like a hang.
    if [ -n "$CURRENT_STRACE_PID" ]; then
        kill -9 "$CURRENT_STRACE_PID" 2>/dev/null
        CURRENT_STRACE_PID=""
    fi
    # Both pids are tracked: $! covers the window before pgrep resolves it, so
    # an interrupt landing between launch and pgrep still reaps the server.
    for p in "$CURRENT_SRV_PID" "$CURRENT_SRV_BG_PID"; do
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    CURRENT_SRV_PID=""
    CURRENT_SRV_BG_PID=""
}

on_signal() {
    local sig=$1
    cleanup
    trap - EXIT INT TERM
    exit $((128 + sig))
}

trap 'cleanup' EXIT
trap 'on_signal 2' INT
trap 'on_signal 15' TERM

TARGET_HOST=${TARGET_HOST:-127.0.0.1}
PORT_START=${PORT_START:-9100}
# Note: no colon in ${VAR-default} -- that form substitutes on an EMPTY value
# too, which would make CONNS_LIST="" (used by `make strace` to skip the timing
# matrix) silently run the full matrix instead.
CONNS_LIST=${CONNS_LIST-"64 512 4096"}
MSG_SIZE=${MSG_SIZE:-64}
WARMUP=${WARMUP:-1}
DURATION=${DURATION:-3}
NODELAY=${NODELAY:-1}
RUNS=${RUNS:-1}
IMPLS=${IMPLS:-"epoll uring"}
# The strace pass counts syscalls, it does not measure throughput, so it gets a
# shorter window: epoll still issues ~13k read/write calls in 1s, and the
# syscalls-per-transaction ratio is scale-invariant.
STRACE_CONNS=${STRACE_CONNS:-64}
STRACE_DURATION=${STRACE_DURATION:-1}
# 2-4 workers saturate the server (~99% CPU); beyond that extra client
# processes only add scheduler/cache contention and *reduce* throughput.
WORKERS=${WORKERS:-4}

SERVER_CPU=${SERVER_CPU:-0}
# cpu1 is the SMT sibling of cpu0 on this box; keep it out of the client set
CLIENT_CPU_BASE=${CLIENT_CPU_BASE:-2}

OUTDIR=${OUTDIR:-results}

mkdir -p "$OUTDIR"

# The timing pass writes the timing file; the strace pass writes its own (see
# run_strace_pass) so the two can never contaminate each other. An empty
# CONNS_LIST means "timing pass only has nothing to do" -- used by `make
# strace` -- and must leave bench.csv completely untouched, header included.
CSV=${RESULTS_CSV:-"$OUTDIR/bench.csv"}
RUN_TIMING=1
[ -z "$CONNS_LIST" ] && RUN_TIMING=0

if [ "$RUN_TIMING" = "1" ]; then
    # Each invocation of the full matrix is a self-contained experiment, so
    # start from a clean file. Set APPEND=1 to merge, or RESULTS_CSV= to
    # redirect (which is how bench-quick avoids clobbering recorded data).
    if [ "${APPEND:-0}" != "1" ] || [ ! -f "$CSV" ]; then
        echo "impl,conns,run,completed,elapsed,rps,server_cpu_pct,client_cpu_pct,workers" > "$CSV"
    fi
    echo "writing to $CSV"
fi

# Resolve the SMT sibling of the server cpu so we can exclude it explicitly.
server_sibling=$(cat "/sys/devices/system/cpu/cpu${SERVER_CPU}/topology/thread_siblings_list" 2>/dev/null || echo "$SERVER_CPU")
NCPU=$(nproc)
LAST_CPU=$((NCPU - 1))
CLIENT_CPU_LIST="${CLIENT_CPU_BASE}-${LAST_CPU}"

# Record the environment alongside the numbers so a CSV is self-describing
# when compared across machines.
{
    echo "# generated $(date -Is)"
    echo "kernel=$(uname -sr)"
    echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "ncpu=$NCPU"
    echo "gcc=$(gcc --version | head -1)"
    echo "liburing=$(ls /lib64/liburing.so.* 2>/dev/null | head -1 | xargs -r basename)"
    echo "runs=$RUNS duration_s=$DURATION warmup_s=$WARMUP workers=$WORKERS msg_size=$MSG_SIZE"
    echo "server_cpu=$SERVER_CPU siblings=$server_sibling client_cpus=$CLIENT_CPU_LIST"
} > "$OUTDIR/env.txt" 2>/dev/null || true

echo "server cpu=$SERVER_CPU siblings=$server_sibling client cpus=${CLIENT_CPU_LIST}"

# distinct port range per impl so a lingering listener can't block the next run
port_for() {
    local impl=$1 conns=$2
    if [ "$impl" = "uring" ]; then
        echo $((PORT_START + 20000 + conns))
    else
        echo $((PORT_START + conns))
    fi
}

# Wait until nothing is listening on $1, so a just-killed server cannot make
# the next run fail to bind.
wait_port_free() {
    local port=$1 tries=0
    while ss -tlnH "sport = :$port" 2>/dev/null | grep -q .; do
        sleep 0.05
        tries=$((tries + 1))
        if [ "$tries" -gt 400 ]; then
            return 1
        fi
    done
}

# Wait until the server is actually accepting on $1. Replaces a fixed sleep
# that was both slow and a race -- it only ever guessed the listener was up.
wait_port_listening() {
    local port=$1 tries=0
    while ! ss -tlnH "sport = :$port" 2>/dev/null | grep -q .; do
        sleep 0.02
        tries=$((tries + 1))
        if [ "$tries" -gt 250 ]; then
            return 1
        fi
    done
}

run_once() {
    local impl=$1 conns=$2 port=$3 dur=$4
    local srv_bin="./build/${impl}_srv"
    local srv_pid
    local strace_out=""
    local strace_pid=""
    wait_port_free "$port"

    # Start the server, then resolve its PID by the unique port rather than
    # trusting $! -- bash sometimes hands back a wrapping subshell (comm=bash),
    # which silently breaks both the /proc CPU sampling and the kill.
    # Redirect the server's output explicitly. A background process that keeps an
    # inherited pipe open makes a $(...) substitution block forever waiting for
    # EOF, which is what turns Ctrl-C into a hang.
    taskset -c "$SERVER_CPU" "$srv_bin" "$port" $((dur + 5)) 500000000 "$NODELAY" \
        >/dev/null 2>&1 &
    CURRENT_SRV_BG_PID=$!
    sleep 0.05
    # Prefer $!: taskset execs the server, so this is the server's own pid.
    # pgrep is only a fallback, and it is racy right after launch, so poll for
    # it briefly rather than reporting a spurious failure when exec is slow.
    srv_pid="$CURRENT_SRV_BG_PID"
    tries=0
    while [ "$tries" -lt 50 ]; do
        found=$(pgrep -f "${srv_bin} ${port} " | head -1)
        if [ -n "$found" ]; then
            srv_pid="$found"
            break
        fi
        kill -0 "$srv_pid" 2>/dev/null || break
        sleep 0.02
        tries=$((tries + 1))
    done
    if ! kill -0 "$srv_pid" 2>/dev/null; then
        echo "  !! server for $srv_bin on port $port did not start" >&2
        srv_pid=0
    fi
    CURRENT_SRV_PID="$srv_pid"

    wait_port_listening "$port"

    if [ "${STRACE:-0}" = "1" ]; then
        strace_out="$OUTDIR/strace_${impl}_${conns}.txt"
        strace -c -o "$strace_out" -p "$srv_pid" >/dev/null 2>"$strace_out.attach" &
        strace_pid=$!
        CURRENT_STRACE_PID="$strace_pid"
        # strace announces the attach on stderr; -o is only written at exit.
        tries=0
        while [ "$tries" -lt 100 ]; do
            if grep -q "attached" "$strace_out.attach" 2>/dev/null; then
                break
            fi
            sleep 0.02
            tries=$((tries + 1))
        done
    fi

    # utime+stime of the server, to prove it (not the client) was the bottleneck
    local cpu_before
    cpu_before=$(awk '{print $14+$15}' "/proc/$srv_pid/stat" 2>/dev/null) || cpu_before=0
    [ -z "$cpu_before" ] && cpu_before=0

    local res
    res=$(timeout $((dur + 30)) taskset -c "$CLIENT_CPU_LIST" \
        ./build/pingpong_cli "$TARGET_HOST" "$port" "$conns" -1 "$MSG_SIZE" \
        "$dur" "$NODELAY" "$WORKERS" "$CLIENT_CPU_BASE" 2>/dev/null | tail -1)

    local cpu_after
    cpu_after=$(awk '{print $14+$15}' "/proc/$srv_pid/stat" 2>/dev/null) || cpu_after=0
    [ -z "$cpu_after" ] && cpu_after=0

    if [ -n "$strace_pid" ]; then
        # SIGINT makes strace detach and flush its -c report
        kill -INT "$strace_pid" 2>/dev/null
        wait "$strace_pid" 2>/dev/null
        CURRENT_STRACE_PID=""
    fi

    kill "$srv_pid" 2>/dev/null
    wait "$srv_pid" 2>/dev/null
    CURRENT_SRV_PID=""
    CURRENT_SRV_BG_PID=""
    wait_port_free "$port"

    # ticks are 1/100s; the server runs a little longer than the client window
    local cpu_pct
    cpu_pct=$(awk -v a="$cpu_after" -v b="$cpu_before" -v d="$dur" \
        'BEGIN{ if (d>0) printf "%.1f", (a-b)/(d*100)*100; else print "NA" }')

    local syscalls=""
    if [ -n "$strace_out" ] && [ -f "$strace_out" ]; then
        # strace -c summary line looks like:
        # 100.00  1.234567  22  50573  23  total
        # the call count is field 4.
        syscalls=$(awk '$NF == "total" {print $4; exit}' "$strace_out" 2>/dev/null)
    fi

    # Results are published as globals, not echoed. run_once must NOT be invoked
    # as $(run_once ...): a command substitution runs it in a subshell, so the
    # CURRENT_SRV_PID it records would never reach the parent's signal handler,
    # and Ctrl-C would orphan the server instead of reaping it.
    RUN_COMPLETED=$(echo "$res" | awk '{print $1}')
    RUN_ELAPSED=$(echo "$res" | awk '{print $2}')
    RUN_CPU_PCT="$cpu_pct"
    # client CPU as % of one core; sum over workers, so 400% means all 4 pegged
    RUN_CLIENT_CPU=$(echo "$res" | awk '{print $3}')
    [ -z "$RUN_CLIENT_CPU" ] && RUN_CLIENT_CPU=0
    RUN_SYSCALLS="${syscalls:-NA}"
}

# Short syscall-counting pass. Kept out of the timing pass because strace makes
# the server an order of magnitude slower; it must not share a file with the
# throughput data or it would drag the rates down.
run_strace_pass() {
    local STRACE=1
    local scsv="$OUTDIR/bench_strace.csv"
    # drop stale reports so a failed pass can never leave old counts looking current
    rm -f "$OUTDIR"/strace_*.txt "$OUTDIR"/strace_*.txt.attach
    echo "impl,conns,completed,elapsed,syscalls" > "$scsv"
    for impl in $IMPLS; do
        local port
        port=$(port_for "$impl" "$STRACE_CONNS")
        local attempt=0 completed elapsed syscalls
        while :; do
            run_once "$impl" "$STRACE_CONNS" "$port" "$STRACE_DURATION"
            completed="$RUN_COMPLETED"
            elapsed="$RUN_ELAPSED"
            [ -z "$completed" ] && completed=0
            [ -z "$elapsed" ] && elapsed=0
            [ "${completed:-0}" -gt 0 ] && break
            attempt=$((attempt + 1))
            [ "$attempt" -ge 3 ] && break
            sleep 1
        done
        syscalls="$RUN_SYSCALLS"
        echo "$impl,$STRACE_CONNS,$completed,$elapsed,${syscalls:-NA}" >> "$scsv"
        printf "  %-5s strace conns=%-5s syscalls=%s\n" "$impl" "$STRACE_CONNS" "${syscalls:-NA}"
        rm -f "$OUTDIR/strace_${impl}_${STRACE_CONNS}.txt.attach"
    done
}

# Timing pass, then a short strace pass so syscall counts are always available.
# The warmup runs once per implementation (at the smallest conn count) rather
# than per point: every run starts a *fresh* server, so a per-point warmup only
# re-warms global state -- caches, slabs, TCP structures -- and measuring in
# ascending order leaves the heaviest point measured last, fully warm.
if [ "$RUN_TIMING" = "1" ]; then
for impl in $IMPLS; do
    warmed=0
    for conns in $CONNS_LIST; do
        port=$(port_for "$impl" "$conns")
        if [ "$warmed" -eq 0 ]; then
            run_once "$impl" "$conns" "$port" "$WARMUP" >/dev/null # discarded
            warmed=1
        fi

        # A point can occasionally come back with 0 completed (a port that has
        # not finished releasing, or a connection burst that outlives the
        # accept queue). Retry rather than record a bogus zero.
        attempt=0
        while :; do
            run_once "$impl" "$conns" "$port" "$DURATION"
            completed="$RUN_COMPLETED"
            elapsed="$RUN_ELAPSED"
            cpu_pct="$RUN_CPU_PCT"
            client_cpu="$RUN_CLIENT_CPU"
            [ -z "$completed" ] && completed=0
            [ -z "$elapsed" ] && elapsed=0

            if [ "${completed:-0}" -gt 0 ]; then
                break
            fi
            attempt=$((attempt + 1))
            if [ "$attempt" -ge 3 ]; then
                echo "  !! $impl conns=$conns: 0 completed after $attempt attempts" >&2
                break
            fi
            echo "  .. $impl conns=$conns: 0 completed, retry $attempt/3" >&2
            sleep 2
        done

        for run in $(seq 1 "$RUNS"); do
            rps=$(awk -v c="$completed" -v t="$elapsed" 'BEGIN{if(t>0)printf "%.1f",c/t;else print 0}')
            echo "$impl,$conns,$run,$completed,$elapsed,$rps,$cpu_pct,$client_cpu,$WORKERS" >> "$CSV"
            printf "  %-5s conns=%-5s run=%s completed=%-9s rps=%-9s srv_cpu=%s%% cli_cpu=%s%%\n" \
                "$impl" "$conns" "$run" "$completed" "$rps" "$cpu_pct" "$client_cpu"
            # more than one sample per point: re-measure each time
            if [ "$run" -lt "$RUNS" ]; then
                run_once "$impl" "$conns" "$port" "$DURATION"
                completed="$RUN_COMPLETED"
                elapsed="$RUN_ELAPSED"
                cpu_pct="$RUN_CPU_PCT"
                client_cpu="$RUN_CLIENT_CPU"
            fi
        done
    done
done

fi # RUN_TIMING

if [ "${SKIP_STRACE:-0}" != "1" ]; then
    STRACE=1 run_strace_pass
fi

if [ "$RUN_TIMING" = "1" ]; then
    echo "results written to $CSV"
fi
