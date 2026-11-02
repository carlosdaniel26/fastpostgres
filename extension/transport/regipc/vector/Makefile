# extension/transport/regipc/vector/Makefile - the ABI-v3 probe: regipcvec.ko and
# its measurement program.
#
#   make                 build the module and fpvec
#   make module          regipcvec.ko only (kbuild against KDIR)
#   make fpvec           the two-process measurement program
#   make load            sudo -n insmod regipcvec.ko width=$(WIDTH) mode=$(MODE)
#   make unload          sudo -n rmmod regipcvec
#   make sweep           the vector-width sweep: 0, 256, 1024, 4096 x REPS, one
#                        JSON line per run, to stdout and sweep.jsonl (needs sudo -n)
#   make bulk            the copy-path sweep (CALL_B): 256, 1024, 4096 x hot/cold
#                        x REPS; needs /dev/regipcvec loaded but no privileges
#   make bulk-sweep      the whole comparison session in one file: bulk hot+cold,
#                        then the vector points reloaded with the same binary,
#                        to stdout and sweep-bulk.jsonl (needs sudo -n)
#   make plain           the 72 B GPR baseline, one process per side
#   make xsave           the no-root userspace xsave+xrstor proxy
#   make selftest        one plain, one vec and one bulk run through the module
#   make clean
#
# The module is its own device (/dev/regipcvec), so it loads beside the ABI 2
# /dev/regipc module this machine already runs. mode=0666 is what lets the
# unprivileged measurement program open it; the default mode of the device
# itself (0600) is changed by the load target only.
#
# WIDTH is a module parameter, not an ioctl argument: one binary, several
# points, and the sweep reloads the module between them (root required).

ifneq ($(KERNELRELEASE),)

obj-m += regipcvec.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
HERE := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO := $(abspath $(HERE)/../../../..)

# the module parameter for `make load`, and the --width `make plain` ignores
WIDTH ?= 1024
MODE  ?= 0666
COUNT ?= 200000
WARMUP ?= 1000
REPS  ?= 3

# bulk-sweep's points: CALL_B's length is per call, so one load serves all of
# the bulk table; the vector points still need a reload each (width is a module
# parameter). Both are inside one session, so the two tables are comparable.
BULK_WIDTHS ?= 256 1024 4096
VEC_WIDTHS  ?= 0 256 1024 4096

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -mgeneral-regs-only
CPPFLAGS += -I$(REPO)/extension/include

SUDO := sudo -n
KO   := $(PWD)/regipcvec.ko

.PHONY: all module fpvec clean load unload sweep bulk bulk-sweep plain xsave selftest

all: module fpvec

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

fpvec: fpvec.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f fpvec sweep.jsonl sweep-bulk.jsonl

# The privilege check is explicit and never prompts: sudo -n fails at once if
# the user has no cached credential, and the message says what to run by hand.
define sudo_check
	@if ! $(SUDO) true 2>/dev/null; then \
		echo "regipcvec: sudo -n is not available (no cached sudo credential)."; \
		echo "run these two commands by hand, as root:"; \
		echo "  sudo insmod $(KO) width=$(WIDTH) mode=$(MODE)"; \
		echo "  sudo rmmod regipcvec"; \
		exit 1; \
	fi
endef

load: module fpvec
	$(sudo_check)
	-$(SUDO) rmmod regipcvec 2>/dev/null
	$(SUDO) insmod $(KO) width=$(WIDTH) mode=$(MODE)
	@echo "regipcvec: loaded, /dev/regipcvec width=$(WIDTH) mode=$(MODE)"

unload:
	$(sudo_check)
	$(SUDO) rmmod regipcvec
	@echo "regipcvec: unloaded"

# The sweep: the module's width is a load-time parameter, so each point is a
# fresh load. The trap makes sure a failed run still unloads.
sweep: module fpvec
	$(sudo_check)
	@set -e; \
	trap '$(SUDO) rmmod regipcvec 2>/dev/null || true' EXIT; \
	for w in $(VEC_WIDTHS); do \
		$(SUDO) rmmod regipcvec 2>/dev/null || true; \
		$(SUDO) insmod $(KO) width=$$w mode=$(MODE); \
		./fpvec --mode vec --width $$w --reps $(REPS) --count $(COUNT) --warmup $(WARMUP); \
	done | tee sweep.jsonl

# The copy-path table alone: CALL_B does not care about the module's width
# parameter, so the module only has to be loaded (any width). No privileges are
# used here; /dev/regipcvec must already exist.
bulk: module fpvec
	@set -e; \
	for w in $(BULK_WIDTHS); do \
		./fpvec --mode bulk --width $$w --reps $(REPS) --count $(COUNT) --warmup $(WARMUP); \
		./fpvec --mode bulk --width $$w --cold --reps $(REPS) --count $(COUNT) --warmup $(WARMUP); \
	done

# The whole comparison in one session and one file: bulk hot+cold first (one
# load), then the vector points reloaded per width with the same binary. The
# trap unloads on any exit.
bulk-sweep: module fpvec
	$(sudo_check)
	@set -e; \
	trap '$(SUDO) rmmod regipcvec 2>/dev/null || true' EXIT; \
	{ \
		$(SUDO) rmmod regipcvec 2>/dev/null || true; \
		$(SUDO) insmod $(KO) width=$(WIDTH) mode=$(MODE); \
		for w in $(BULK_WIDTHS); do \
			./fpvec --mode bulk --width $$w --reps $(REPS) --count $(COUNT) --warmup $(WARMUP); \
			./fpvec --mode bulk --width $$w --cold --reps $(REPS) --count $(COUNT) --warmup $(WARMUP); \
		done; \
		for w in $(VEC_WIDTHS); do \
			$(SUDO) rmmod regipcvec 2>/dev/null || true; \
			$(SUDO) insmod $(KO) width=$$w mode=$(MODE); \
			./fpvec --mode vec --width $$w --reps $(REPS) --count $(COUNT) --warmup $(WARMUP); \
		done; \
	} | tee sweep-bulk.jsonl

plain: fpvec
	./fpvec --mode plain --reps $(REPS) --count $(COUNT) --warmup $(WARMUP)

xsave: fpvec
	./fpvec --mode xsave --reps $(REPS)

selftest: fpvec
	./fpvec --selftest --width $(WIDTH)

endif

chart:
	python3 chart.py --out .
