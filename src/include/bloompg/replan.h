#ifndef BLOOMPG_REPLAN_H
#define BLOOMPG_REPLAN_H

#include "postgres.h"

#include "bloompg/transfer.h"

extern void bloompg_replan_init(void);
extern void bloompg_replan_fini(void);
extern void bloompg_replan_begin(List *replacements,
								 const BloomPGJoinGraph *graph);
extern void bloompg_replan_end(void);
extern void bloompg_replan_cardinality_stats(uint64 *adjustments,
										 double *max_ratio);

#endif
