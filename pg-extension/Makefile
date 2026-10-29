EXTENSION = nativepostgress
MODULE_big = nativepostgress
OBJS = nativepostgress.o
DATA = nativepostgress--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
