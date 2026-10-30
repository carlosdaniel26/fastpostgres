EXTENSION = fastpostgres
MODULE_big = fastpostgres
OBJS = src/fastpostgres.o src/columnar.o src/shm.o src/write.o src/stream.o src/worker.o
DATA = fastpostgres--1.0.sql

PG_CONFIG = pg_config

# -O3 for the serialization hot loops (PGXS defaults to -O2).
PG_CFLAGS += -O3

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
