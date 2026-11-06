# The extension: PGXS, one shared header, SQL in sql/, and pg_regress tests.
#
#   make && make install          build and install the module
#   make installcheck             run test/sql against a running server
#
# The wire format the worker writes and the client reads lives in
# include/fastpostgres/, one copy for both sides of the ring.

EXTENSION = fastpostgres
MODULE_big = fastpostgres

OBJS = src/fastpostgres.o src/columnar.o src/columnar_store.o src/rows.o src/shm.o src/stream.o \
       src/reduce.o src/worker.o \
       src/carry.o transport/transport.o transport/engine_shm.o transport/engine_eventfd.o

# PostgreSQL wants a version in fastpostgres.control and wants the SQL file named
# after it. Nothing is released, so both say 0.0.0: no upgrade paths to carry, and
# no place else in the tree states a version.
DATA = sql/fastpostgres--0.0.0.sql

REGRESS = fastpostgres
REGRESS_OPTS = --inputdir=test

PG_CONFIG = pg_config
PG_CPPFLAGS += -I$(srcdir)/include -I$(srcdir) -I$(srcdir)/src

# The extension is C++23 (src/pgxx.h is the bridge to PostgreSQL's C API).
# -O3 matches the serialization hot loops' setting (PGXS defaults to -O2);
# -fno-exceptions keeps a C++ throw from unwinding through a PostgreSQL frame,
# whose error path is a longjmp rather than an exception.
PG_CXXFLAGS += -std=c++23 -O3 -fno-exceptions

# -O3 for the serialization hot loops (PGXS defaults to -O2).
PG_CFLAGS += -O3

# Nothing here is worth inlining into JIT'd expressions: the SQL surface is two
# ring functions, and the hot loops run inside the worker, not in the executor.
override with_llvm := no

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
