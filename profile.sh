#!/bin/bash
# CPU profile of a benchmark server.
#
# Usage: ./profile.sh [impl] [mode]
#   impl: uring | epoll          (default uring)
#   mode: fp | flat | stat   (default fp)
#
# Everything else is an env var, matching bench.sh:
#   CONNS DURATION MSG_SIZE NODELAY SERVER_CPU CLIENT_CPU_BASE IMPLS PROTO
#   OUTDIR PORT_START FREQ
#
# Silent on success: it prints the client tally and the hot-symbol table, and
# nothing else. The checks in preflight/report_profile stay quiet unless one of
# them fails, which is the only time the number would have been a lie --
# an unprivileged profile (perf_event_paranoid>=2) silently drops kernel samples,
# so it can print a table that describes no kernel time at all.
#
# Unwinding: 'fp' is the only stack mode. It sustains 1999 Hz, and the build
# carries -fno-omit-frame-pointer -fno-optimize-sibling-calls unconditionally so
# the %rbp chain is always intact.
set -uo pipefail

IMPL=${1:-uring}
MODE=${2:-fp}

SERVER_CPU=${SERVER_CPU:-0}
CLIENT_CPU_BASE=${CLIENT_CPU_BASE:-2}
CONNS=${CONNS:-512}
MSG_SIZE=${MSG_SIZE:-64}
DURATION=${DURATION:-5}
NODELAY=${NODELAY:-1}
PROTO=${PROTO:-tcp}
# Must stay inside 16 bits: the servers bind with htons((uint16_t)port), so a
# port above 65535 is silently truncated and the client truncates identically --
# the run appears to work while every port check looks for a port nobody bound.
PORT=${PORT:-$((40000 + CONNS))}
OUTDIR=${OUTDIR:-results/prof}
# 0 means "let the mode pick": fp and flat want different rates.
FREQ=${FREQ:-0}

CLIENT_CPU_LIST="${CLIENT_CPU_BASE}-$(($(nproc) - 1))"
SRV="./build/${IMPL}_srv"
CLI="./build/pingpong_cli"

die() {
    echo "!! $*" >&2
    exit 1
}

usage() {
    awk 'NR == 1 { next } /^[^#]/ { exit } { sub(/^# ?/, ""); print }' "$0"
    exit "${1:-0}"
}

# ---------------------------------------------------------------- preflight --

preflight() {
    command -v perf >/dev/null 2>&1 || die "perf not found (install linux-tools/perf)"

    case "$MODE" in fp|flat|stat) ;; *) die "unknown mode '$MODE'" ;; esac
    case "$IMPL" in uring|epoll) ;; *) die "unknown impl '$IMPL'" ;; esac

    # Root, or perf_event_paranoid>=2 makes perf append ":u" to every event and
    # drop the kernel entirely, and kptr_restrict zeroes /proc/kallsyms so a
    # sampled kernel IP could not be named anyway.
    if [ "$(id -u)" -eq 0 ]; then
        SUDO=""
    elif sudo -n true 2>/dev/null; then
        SUDO="sudo -n"
    else
        die "need root (or passwordless sudo) for kernel attribution.
   perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null) and
   kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null) mean an
   unprivileged profile would silently contain NO kernel samples."
    fi

    [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || \
        die "PORT=$PORT is outside 1-65535; htons((uint16_t)port) would truncate it"
    [ -x "$SRV" ] || die "$SRV not built (run: make)"
    [ -x "$CLI" ] || die "$CLI not built"
}

# ------------------------------------------------------------------ helpers --

# Resolve the server pid without pgrep: $! is the backgrounded command and
# taskset execs into it, but bash sometimes wraps the command in a subshell whose
# comm is bash. Polling /proc/PID/comm until it names the binary avoids matching
# a leftover server from an earlier phase.
resolve_srv_pid() {
    local want=$1 tries=0 comm
    comm=$(basename "$SRV")
    while [ "$tries" -lt 100 ]; do
        if [ "$(cat "/proc/$1/comm" 2>/dev/null)" = "$comm" ]; then
            echo "$1"
            return 0
        fi
        kill -0 "$1" 2>/dev/null || return 1
        sleep 0.02
        tries=$((tries + 1))
    done
    return 1
}

wait_port() {
    local port=$1 filter="-tlnH" tries=0
    [ "$PROTO" = "udp" ] && filter="-ulnH"
    while ! ss $filter "sport = :$port" 2>/dev/null | grep -q .; do
        sleep 0.02
        tries=$((tries + 1))
        [ "$tries" -gt 250 ] && return 1
    done
    return 0
}

# --------------------------------------------------------------------- modes --

# Build the perf argument list. Kept explicit rather than clever so that a typo
# shows up in the logged command rather than as a subtly wrong profile.
record_args() {
    # FREQ=0 means "use this mode's default". Note ${FREQ:-N} would NOT do that:
    # 0 is non-empty, so it would be passed through literally as "-F 0".
    local f=${FREQ:-0}
    case "$MODE" in
    flat)  [ "$f" -eq 0 ] && f=1999; echo "-F $f" ;;
    fp)    [ "$f" -eq 0 ] && f=1999; echo "-F $f --call-graph fp" ;;
    stat)  echo "" ;;
    esac
}

run_profile() {
    local srv_pid=$1
    local data="$OUTDIR/perf.data" log
    log="$OUTDIR/perf.log"

    case "$MODE" in
    stat)
        echo "--- perf stat: counters over ${DURATION}s ---"
        $SUDO perf stat -o "$OUTDIR/stat.txt" -p "$srv_pid" \
            -e task-clock,context-switches,cpu-migrations,page-faults \
            -- sleep "$DURATION" 2>"$log" &
        ;;
    *)
        echo "--- perf record: mode=$MODE args=$(record_args) over ${DURATION}s ---"
        # Recording runs in the background so the client starts *inside* the
        # window. Recording first and starting the client afterwards profiles an
        # idle server, which is a real mistake this script is written to avoid.
        $SUDO perf record -o "$data" -p "$srv_pid" -e cpu-clock $(record_args) \
            -- sleep "$DURATION" 2>"$log" &
        ;;
    esac
    local perf_bg=$!
    sleep 0.3

    taskset -c "$CLIENT_CPU_LIST" "$CLI" 127.0.0.1 "$PORT" "$CONNS" -1 "$MSG_SIZE" \
        "$DURATION" "$NODELAY" 4 "$CLIENT_CPU_BASE" "$PROTO" >"$OUTDIR/client.out" 2>/dev/null
    echo "client: $(cat "$OUTDIR/client.out")"

    wait "$perf_bg" 2>/dev/null
    grep -oE 'wrote .*\([0-9]+ samples\)' "$log" | tail -1 | sed 's/^/  /' 

    # Give the server its full duration so it exits and prints its own tally,
    # then reap. Escalate to SIGKILL: a server parked in io_uring_enter can miss
    # SIGTERM, and a lingering socket would let the next run share the port.
    sleep 0.3
    kill -TERM "$srv_pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$srv_pid" 2>/dev/null || break
        sleep 0.02
    done
    kill -KILL "$srv_pid" 2>/dev/null
    wait "$srv_pid" 2>/dev/null
    return 0
}

# Render one perf report view as a compact symbol list.
#
# Two filters, both load-bearing:
#   * symbol rows only, then truncate -- perf report emits a *call tree* whose
#     first subtree runs to ~105 lines, so `head` before `grep` showed one symbol
#     out of fifteen.
#   * trailing IPC columns dropped when they carry no data. There is no PMU on
#     this box, so every row ended in two meaningless dashes.
render_view() {
    local file=$1 title=$2 drop_unresolved=$3
    echo
    echo "$title"
    awk -v n="${TOPN:-20}" -v drop="$drop_unresolved" '
        /^[[:space:]]+[0-9]+\.[0-9]+%/ {
            if (NF >= 2 && $(NF - 1) == "-" && $NF == "-") m = NF - 2; else m = NF
            sym = ""
            for (i = 1; i < m; i++)
                if ($i ~ /^\[[k.]\]$/) { sym = $(i + 1); break }
            # Unsymbolised frames are real data, so they are not hidden: counted
            # and disclosed, not silently dropped.
            if (drop && sym ~ /^0x[0-9a-f]+$/) { unresolved++; next }
            line = ""
            for (i = 1; i <= m; i++) line = line (i > 1 ? "  " : "") $i
            print line
            if (++shown >= n) exit
        }
        END {
            if (unresolved)
                printf "  (%d unresolved frame%s omitted)\n",
                       unresolved, (unresolved == 1 ? "" : "s")
        }' "$file"
}

# ------------------------------------------------------------------ report ---

# Silent when the artefact can support a claim, fatal when it cannot. A profile
# with no kernel samples (the perf_event_paranoid ":u" trap) or one-frame stacks
# (unwinding failed) still prints a confident-looking table while describing
# almost none of the time actually spent, so those two are hard errors.
report_profile() {
    [ "$MODE" = "stat" ] && { cat "$OUTDIR/stat.txt"; return 0; }

    local data="$OUTDIR/perf.data" script="$OUTDIR/perf.script"
    [ -s "$data" ] || die "no perf.data produced"

    $SUDO perf script -i "$data" >"$script" 2>/dev/null || die "perf script failed"

    # Count SAMPLES, not kernel frames: one sample contributes up to ~55 kernel
    # lines, so grep -c over the whole file overshoots by an order of magnitude.
    # Sample shape differs by mode, so the counting differs too:
    #   fp:    one non-indented header line per sample, then indented frame
    #          lines. Samples are counted from the headers.
    #   flat:  no call graph, so each sample is a *single indented* line and
    #          there are no header lines at all. Counting headers yields 0 here,
    #          which aborts the run with "perf.data contains no samples" on a
    #          perfectly good profile -- so count lines instead.
    local counts total kern
    if [ "$MODE" = "flat" ]; then
        counts=$(awk '
            /^[^ \t]/ { next }
            /[^ \t]/ {
                tot++
                if ($0 ~ /kernel\.kallsyms/) kern++
            }
            END { printf "%d %d\n", tot, kern }' "$script")
    else
        counts=$(awk '
            /^[^ \t]/ { if (st) { tot++; if (hk) kern++ } ; st=1; hk=0; next }
            /^[ \t]/ { if ($0 ~ /kernel\.kallsyms/) hk=1 }
            END { if (st) { tot++; if (hk) kern++ }
                  printf "%d %d\n", tot, kern }' "$script")
    fi
    total=${counts%% *}
    kern=${counts##* }
    [ "${total:-0}" -gt 0 ] || die "perf.data contains no samples (was the server idle?)"
    [ "$kern" -gt 0 ] || die "profile has NO kernel samples -- this is the
   perf_event_paranoid ':u' trap, or the server never left userspace."

    if [ "$MODE" != "flat" ]; then
        local mean_depth
        mean_depth=$(awk '
            /^[^ \t]/ { if (d) { s+=d; n++ } ; d=0 ; next }
            /^[ \t]/ { d++ }
            END { if (d) { s+=d; n++ } ; printf "%.1f", (n? s/n : 0) }' "$script")
        # Unwinding that is broken yields depth 1-2; real kernel stacks here are
        # ~28 frames, so anything under 5 means the call-graph method failed.
        awk -v d="$mean_depth" 'BEGIN{exit !(d < 5)}' && \
            die "mean stack depth $mean_depth looks like unwinding failed (mode=$MODE)"
    fi

    # Two views, because they answer different questions. --children is
    # inclusive: how much time is spent *under* a symbol. --no-children is
    # exclusive: how much a symbol burns on its own.
    #
    # Only the inclusive one localises the cost here. It puts 95.39% of samples
    # under __io_issue_sqe and 79.77% under tcp_sendmsg_locked, which is the
    # whole story of where an io_uring server spends a transaction. The
    # exclusive view puts _raw_spin_unlock_irqrestore at the top, a leaf nobody
    # can act on, and its top 20 symbols sum to well under a quarter of the
    # profile -- so it ranks call sites, not costs.
    # A flat profile has no call graph, so there are no children to accumulate:
    # the inclusive column would just restate the exclusive one. Skip it there.
    if [ "$MODE" != "flat" ]; then
        $SUDO perf report -i "$data" -f --stdio --children \
            --percent-limit "${PCTLIMIT:-1.0}" --sort symbol \
            >"$OUTDIR/report-cumulative.txt" 2>/dev/null
        render_view "$OUTDIR/report-cumulative.txt" "cumulative (inclusive):" 1
    fi

    $SUDO perf report -i "$data" -f --stdio --no-children \
        --percent-limit "${PCTLIMIT:-1.0}" --sort symbol \
        >"$OUTDIR/report-self.txt" 2>/dev/null
    render_view "$OUTDIR/report-self.txt" "self (exclusive):" 0
}

# -------------------------------------------------------------------- main ---

main() {
    [ "${1:-}" = "-h" ] && usage 0
    preflight

    mkdir -p "$OUTDIR"
    echo "impl=$IMPL mode=$MODE proto=$PROTO conns=$CONNS dur=${DURATION}s"

    local stray
    stray=$(pgrep -x "${IMPL}_srv" 2>/dev/null | tr '\n' ' ')
    [ -n "$stray" ] && die "stray ${IMPL}_srv already running (pids:$stray) -- a second
   server would share the port (SO_REUSEADDR allows it on udp) and split the
   traffic, invalidating the profile. Kill it first."

    taskset -c "$SERVER_CPU" "$SRV" "$PORT" $((DURATION + 10)) 500000000 "$NODELAY" "$PROTO" \
        >/dev/null 2>"$OUTDIR/server.err" &
    local bg=$!
    local srv_pid
    srv_pid=$(resolve_srv_pid "$bg") || die "server did not start"
    wait_port "$PORT" || die "server never bound port $PORT"

    run_profile "$srv_pid"
    report_profile
}

main "$@"