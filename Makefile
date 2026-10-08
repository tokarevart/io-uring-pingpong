# Frame pointers are unconditional: -fno-omit-frame-pointer establishes the %rbp
# chain that `perf record -g fp` needs (gcc omits frame pointers at -O3 by
# default, leaving 1-2 setups across ~40 symbols), and
# -fno-optimize-sibling-calls stops tail calls reusing the caller's frame and
# severing that chain mid-stack. gcc leaves sibling calls enabled even with frame
# pointers, so asking for only the first flag yields stacks that are silently
# truncated at tail-call sites. Neither flag is substitutable for the other.
#
# They cost nothing measurable here (-0.8% to +2.6% across the four
# impl/transport combinations, all inside run-to-run noise), because at -O3
# these servers inline down to ~11 symbols and userspace is ~0.3% of samples.
CFLAGS = -std=gnu23 -Wall -Wextra -Werror -Wno-gnu-folding-constant -g \
         -fno-omit-frame-pointer -fno-optimize-sibling-calls
LDFLAGS_URING = -luring

.PHONY: all build prof bench bench-quick bench-udp report strace clean distclean fmt

all: build

build: build/epoll_srv build/uring_srv build/pingpong_cli

build/epoll_srv: epoll_srv.c
build/uring_srv: uring_srv.c
build/pingpong_cli: pingpong_cli.c

build/%: %.c
	mkdir -p build
	gcc $(CFLAGS) -O3 $< $(LIBS) -o $@

build/uring_srv: LIBS = $(LDFLAGS_URING)

# Timing matrix for both transports + short syscall-counting passes, then the
# tables. Sweeps tcp and udp by default (PROTO_LIST); they write separate CSVs
# and report.py prints separate tables, since udp rps is not comparable.
# Overrides (DURATION, CONNS_LIST, ...) are all honoured, but the defaults are
# deliberately short -- one 3s run per point, so a full sweep is about a minute.
bench: build
	./bench.sh
	./report.py

# UDP only: one transport, for when iterating on the datagram path and you do
# not want to pay for the tcp sweep too.
bench-udp: build
	PROTO_LIST=udp ./bench.sh
	./report.py

# Re-run only the syscall-counting pass (skips the timing matrix entirely).
strace: build
	CONNS_LIST="" ./bench.sh
	./report.py

# Single point, genuinely quick. Usage: make bench-quick CONNS=1024 IMPLS=uring
# PROTO=tcp|udp picks the transport (default tcp).
# Runs from a temporary file that is deleted afterwards, so a quick check never
# persists anything and can never alter the recorded results/bench.csv.
CONNS ?= 1024
IMPLS ?= "uring epoll"
PROTO ?= tcp
bench-quick: build
	@tmp=$$(mktemp); \
	trap 'rm -f "$$tmp"; exit 130' INT TERM; \
	trap 'rm -f "$$tmp"' EXIT; \
	CONNS_LIST=$(CONNS) IMPLS=$(IMPLS) WARMUP=1 DURATION=3 SKIP_STRACE=1 \
		RESULTS_CSV="$$tmp" PROTO_LIST=$(PROTO) ./bench.sh; \
	./report.py "$$tmp" "" "quick run"

# CPU profile of a server. Override with `make prof IMPL=epoll MODE=stat`.
IMPL ?= uring
MODE ?= fp
prof: build
	@./profile.sh $(IMPL) $(MODE)

report:
	./report.py

fmt:
	clang-format -i *.c *.h 2>/dev/null || true

# `clean` keeps results/ on purpose: bench.csv is the recorded evidence.
clean:
	rm -rf build

# distclean drops recorded results too.
distclean:
	rm -rf build results
