# The extension: PGXS, one shared header, SQL in sql/, and pg_regress tests.
#
#   make && make install          build and install the module
#   make installcheck             run test/sql against a running server
#
# The wire format the worker writes and the client reads lives in
# include/fastpostgres/, one copy for both sides of the ring.

EXTENSION = fastpostgres
MODULE_big = fastpostgres

OBJS = src/fastpostgres.o src/columnar.o src/rows.o src/shm.o src/stream.o src/worker.o

# The version lives in this file and in fastpostgres.control, and the SQL file is
# named after it; `just check-version` proves the three agree.
DATA = sql/fastpostgres--1.0.0.sql

REGRESS = fastpostgres
REGRESS_OPTS = --inputdir=test

PG_CONFIG = pg_config
PG_CPPFLAGS += -I$(srcdir)/include

# -O3 for the serialization hot loops (PGXS defaults to -O2).
PG_CFLAGS += -O3

# Nothing here is worth inlining into JIT'd expressions: the SQL surface is two
# ring functions, and the hot loops run inside the worker, not in the executor.
override with_llvm := no

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
