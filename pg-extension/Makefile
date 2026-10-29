EXTENSION = nativepostgress
MODULE_big = nativepostgress
OBJS = src/nativepostgress.o src/columnar.o src/shm.o src/write.o src/stream.o src/worker.o
DATA = nativepostgress--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
