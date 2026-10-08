#!/bin/bash
# CPU profile of a benchmark server, with the gates that make the result mean
# something.
#
# Usage: ./profile.sh [impl] [mode]
#   impl: uring | epoll          (default uring)
#   mode: fp | flat | stat   (default fp)
#
# Everything else is an env var, matching bench.sh:
#   CONNS DURATION MSG_SIZE NODELAY SERVER_CPU CLIENT_CPU_BASE IMPLS PROTO
#   OUTDIR PORT_START FREQ
#
# Why the gates exist. This script has been written after getting a profile
# wrong three separate ways, every one of them silent:
#   * `perf -g none` -- perf parses "none" as a workload *name*, reports
#     "Failed to collect", and produces no samples.
#   * recording before starting the client -- perf happily profiles an idle
#     server and writes a small, perfectly valid, useless file.
#   * unprivileged perf -- perf_event_paranoid=2 makes perf silently append
#     ":u" to every event and drop all kernel samples, so a profile can come
#     back 100% "successful" while describing no kernel time at all.
# So: check the privilege gates up front and refuse; then verify after the fact
# that kernel samples actually landed and that stacks actually unwound.
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
    sed -n '2,30p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

# ---------------------------------------------------------------- preflight --

preflight() {
    command -v perf >/dev/null 2>&1 || die "perf not found (install linux-tools/perf)"

    case "$MODE" in fp|flat|stat) ;; *) die "unknown mode '$MODE'" ;; esac
    case "$IMPL" in uring|epoll) ;; *) die "unknown impl '$IMPL'" ;; esac

    # Privilege. Without root, perf_event_paranoid >= 2 makes perf append ":u"
    # to every event and exclude the kernel entirely; kptr_restrict also zeroes
    # the addresses in /proc/kallsyms so a sampled kernel IP could not be named
    # even if it were collected. Both gates vanish at uid 0, so this is a hard
    # requirement, not a nicety.
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

    local paranoid
    paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "?")
    echo "preflight: root=yes  perf_event_paranoid=$paranoid  kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)"

    # WSL2 exposes no virtualised PMU, so `cycles` is unavailable and cpu-clock
    # software sampling is the only option. Detect rather than assume, since a
    # PMU-less VM reports "<not supported>" rather than failing.
    local probe
    probe=$($SUDO perf stat -e cycles -- true 2>&1 || true)
    if echo "$probe" | grep -q "not supported"; then
        echo "preflight: no hardware PMU (cycles unsupported) -> cpu-clock software sampling"
    elif echo "$probe" | grep -qE "[0-9]"; then
        echo "preflight: hardware cycles available (still using cpu-clock, for comparability)"
    else
        die "could not determine PMU support; perf said: $(echo "$probe" | tr '\n' ' ')"
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
    sed -n 's/.*wrote/  perf: wrote/p' "$log" | tail -1

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

# ----------------------------------------------------------------- verify ----

# A profile that recorded nothing, or nothing from the kernel, or stacks one
# frame deep, is not a result. Check the artefact rather than the exit status.
verify_profile() {
    [ "$MODE" = "stat" ] && { cat "$OUTDIR/stat.txt"; return 0; }

    local data="$OUTDIR/perf.data" script="$OUTDIR/perf.script"
    [ -s "$data" ] || die "no perf.data produced"

    $SUDO perf script -i "$data" >"$script" 2>/dev/null || die "perf script failed"

    # Count SAMPLES, not kernel frames: one sample contributes up to ~55 kernel
    # lines, so grep -c over the whole file overshoots by an order of magnitude.
    local counts total kern usr mean_depth
    # Sample shape differs by mode, so the counting has to differ too:
    #   fp:    one non-indented header line per sample, then indented frame
    #          lines. Samples are counted from the headers.
    #   flat:  no call graph, so each sample is a *single indented* line and
    #          there are no header lines at all. Counting headers yields 0 here,
    #          which aborts the run with "perf.data contains no samples" on a
    #          perfectly good profile -- so count lines instead.
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

    usr=$((total - kern))
    echo "verify: $total samples, $kern with kernel frames, $usr userspace-only"

    # The important gate. A userspace-only profile is what an unprivileged or
    # paranoid>=2 run silently produces, and for this workload it describes
    # under 1% of the time actually spent.
    if [ "$kern" -eq 0 ]; then
        die "profile has NO kernel samples -- this is the perf_event_paranoid ':u'
   trap, or the server never left userspace. Refusing to report it."
    fi
    local kpct=$((100 * kern / total))
    [ "$kpct" -lt 50 ] && echo "verify: WARNING only ${kpct}% of samples have kernel frames"

    if [ "$MODE" != "flat" ]; then
        mean_depth=$(awk '
            /^[^ \t]/ { if (d) { s+=d; n++ } ; d=0 ; next }
            /^[ \t]/ { d++ }
            END { if (d) { s+=d; n++ } ; printf "%.1f", (n? s/n : 0) }' "$script")
        echo "verify: mean stack depth = $mean_depth frames"
        # Unwinding that is broken yields depth 1-2; real kernel stacks here are
        # ~28 frames, so anything under 5 means the call-graph method failed.
        awk -v d="$mean_depth" 'BEGIN{exit !(d < 5)}' && \
            die "mean stack depth $mean_depth looks like unwinding failed (mode=$MODE)"
    fi
    echo "verify: ok"

    $SUDO perf report -i "$data" -f --stdio --no-children \
        --percent-limit "${PCTLIMIT:-1.0}" --sort symbol >"$OUTDIR/report.txt" 2>/dev/null
    echo "report: $OUTDIR/report.txt"
    head -40 "$OUTDIR/report.txt" | grep -E "^\s+[0-9]+\.[0-9]+%" || true
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
    verify_profile
    echo "artifacts in $OUTDIR/"
}

main "$@"