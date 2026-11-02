# benchmarks/internals/transport/Makefile — the doorbell peers of the ring's two
# transports (peer.c), built against the product's own engines.
#
# Needs no PostgreSQL and no PGXS: transport.c, engine_shm.c and engine_regipc.c
# have no server dependency, so the engines compile with plain gcc/clang on any
# x86-64 Linux host (the extension itself needs PGXS; this does not).
#
#   make bench      build build/peer (git-ignored; see the repository .gitignore)
#   make selftest   build, then one hop per transport, PASS/FAIL lines
#   make clean      remove build/
#
# The host harness is benchmarks/jobs/transport.sh; it guards the two runtime
# prerequisites (/dev/regipc, and this binary) with named remedies.

here := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
repo := $(abspath $(here)/../../..)
out  := $(here)/build
bin  := $(out)/peer
srcs := $(here)/peer.c \
        $(repo)/extension/transport/transport.c \
        $(repo)/extension/transport/engine_shm.c \
        $(repo)/extension/transport/engine_regipc.c

CC       ?= cc
CPPFLAGS += -I$(repo)/extension/transport -I$(repo)/extension/include
CFLAGS   ?= -O3 -g
CFLAGS   += -Wall -Wextra
LDLIBS   := -pthread

.PHONY: bench selftest clean

bench: $(bin)

$(bin): $(srcs)
	@mkdir -p $(out)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(srcs) $(LDLIBS)

selftest: $(bin)
	$(bin) --selftest

clean:
	rm -rf $(out)
