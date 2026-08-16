#ifndef BLOOMPG_SCAN_QUALS_H
#define BLOOMPG_SCAN_QUALS_H

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"

/* Return immutable base-table semantics detached from a physical scan path. */
extern bool bloompg_scan_has_base_slot_semantics(const Plan *scan);
extern List *bloompg_scan_base_quals(Plan *scan);
extern SeqScan *bloompg_make_seqscan_evaluator(Plan *scan);

#endif
