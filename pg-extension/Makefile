EXTENSION = fastpostgres
MODULE_big = fastpostgres

# The marshalling kernels live in ../simd and are shared with the Python client:
# they are freestanding (no Postgres headers), so both sides compile the same
# sources, exactly as they already share fastpostgres_shm.h.
SIMD_DIR = $(CURDIR)/../simd
SIMD_OBJS = $(SIMD_DIR)/cpu.o $(SIMD_DIR)/kernels.o $(SIMD_DIR)/kernels_avx2.o $(SIMD_DIR)/kernels_avx512.o

OBJS = src/fastpostgres.o src/columnar.o src/shm.o src/write.o src/stream.o src/worker.o $(SIMD_OBJS)
DATA = fastpostgres--1.0.sql

PG_CONFIG = pg_config

# -O3 for the serialization hot loops (PGXS defaults to -O2). The include goes
# in PG_CPPFLAGS, not PG_CFLAGS: the LLVM bitcode pass PGXS runs for JIT only
# picks up CPPFLAGS, and it needs kernels.h too.
PG_CFLAGS += -O3
PG_CPPFLAGS += -I$(SIMD_DIR)

# -fPIC matters more than usual here: a shared object with a text relocation is
# refused by the backend, so the assembly is written position independent and
# compiled that way.
$(SIMD_DIR)/%.o: $(SIMD_DIR)/%.c
	$(CC) -c -O3 -fPIC -I$(SIMD_DIR) $(CPPFLAGS) -o $@ $<

$(SIMD_DIR)/%.o: $(SIMD_DIR)/%.S
	$(CC) -c -fPIC -I$(SIMD_DIR) $(CPPFLAGS) -o $@ $<

# PGXS wants a JIT bitcode file for every object, and there is no bitcode for
# assembly. Nothing here is worth inlining anyway: the SQL surface is two ring
# functions, and the hot loops are all in C or asm.
override with_llvm := no

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
