CFLAGS = -std=gnu23 -Wall -Wextra -Werror -Wno-gnu-folding-constant -g
LDFLAGS_URING = -luring

.PHONY: all build bench bench-quick bench-udp report strace clean distclean fmt

all: build

build: build/epoll_srv build/uring_srv build/pingpong_cli

build/epoll_srv: epoll_srv.c
	mkdir -p build
	gcc $(CFLAGS) -O3 $< -o $@

build/uring_srv: uring_srv.c
	mkdir -p build
	gcc $(CFLAGS) -O3 $< $(LDFLAGS_URING) -o $@

build/pingpong_cli: pingpong_cli.c
	mkdir -p build
	gcc $(CFLAGS) -O3 $< -o $@

# Timing matrix for both transports + short syscall-counting passes, then the
# tables. Sweeps tcp and udp by default (PROTO_LIST); they write separate CSVs
# and report.py prints separate tables, since udp rps is not comparable.
# Overrides (RUNS, DURATION, CONNS_LIST, ...) are all honoured, but the defaults
# are deliberately short -- keep end-to-end runs well under a minute.
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
	CONNS_LIST=$(CONNS) IMPLS=$(IMPLS) RUNS=1 WARMUP=1 DURATION=3 SKIP_STRACE=1 \
		RESULTS_CSV="$$tmp" PROTO_LIST=$(PROTO) ./bench.sh; \
	./report.py "$$tmp" "" "quick run"

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
