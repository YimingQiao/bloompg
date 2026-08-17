EXTENSION = bloompg
MODULE_big = bloompg
PGFILEDESC = "bloompg - adaptive Bloom-filter transfer for PostgreSQL"
.DEFAULT_GOAL := all

OBJS = \
	src/bloompg.o \
	src/planner.o \
	src/graph.o \
	src/transfer_plan.o \
	src/transfer_access.o \
	src/transfer_index.o \
	src/transfer_materialize.o \
	src/transfer_parallel.o \
	src/transfer_scan.o \
	src/transfer_prefetch.o \
	src/transfer_compact.o \
	src/transfer_statistics.o \
	src/transfer_domain.o \
	src/transfer_finalize.o \
	src/transfer_profile.o \
	src/transfer.o \
	src/transfer_cleanup.o \
	src/path_transform.o \
	src/replan.o \
	src/profile.o \
	src/sampling.o \
	src/scan_quals.o \
	src/scan_plan.o \
	src/scan_apply.o \
	src/scan_parallel.o \
	src/scan_pack.o \
	src/scan_memory.o \
	src/scan_tid.o \
	src/scan_filter.o \
	src/filter.o \
	src/column_store.o \
	src/statistics.o \
	src/runtime.o

DATA = \
	sql/bloompg--0.1.0.sql \
	sql/bloompg--0.1.1.sql \
	sql/bloompg--0.1.2.sql \
	sql/bloompg--0.1.0--0.1.1.sql \
	sql/bloompg--0.1.1--0.1.2.sql
REGRESS = bloompg
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CPPFLAGS = -Isrc/include
PG_CFLAGS = -std=gnu11 -Wall -Wextra -Werror=vla -Wno-clobbered

# PGXS does not consistently emit C header dependency files.  BloomPG's
# private interface is small enough that rebuilding extension objects after a
# header edit is preferable to ever linking stale ABI assumptions.
$(OBJS): $(wildcard src/include/bloompg/*.h)

PG_CONFIG ?= pg_config
PG_VERSION := $(word 2,$(shell $(PG_CONFIG) --version 2>/dev/null))
PG_MAJOR := $(firstword $(subst ., ,$(PG_VERSION)))
ifneq ($(PG_MAJOR),18)
$(error BloomPG requires PostgreSQL 18; set PG_CONFIG=/path/to/pg18/bin/pg_config)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

.PHONY: pythoncheck
pythoncheck:
	python3 -m unittest discover -s test/python -v
