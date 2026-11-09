#ifndef BLOOMPG_WORKER_BUILD_H
#define BLOOMPG_WORKER_BUILD_H

#include "postgres.h"
#include "bloompg/transfer_internal.h"

extern List *bloompg_worker_build_metadata(List *pending, int workers,
	BloomPGMemoryBudget *budget, Size *reserved);
extern List *bloompg_worker_build_begin(List *metadata);
extern bytea *bloompg_worker_build_packet(List *pending, List *metadata,
	int *index, uint64 *offset, double build_ms);
extern bool bloompg_worker_build_receive(Datum datum, List *pending,
	BloomPGRelationState *state);

#endif
