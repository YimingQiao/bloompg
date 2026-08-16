#ifndef BLOOMPG_PATH_TRANSFORM_H
#define BLOOMPG_PATH_TRANSFORM_H

#include "postgres.h"

#include "bloompg/transfer.h"

#include "nodes/pathnodes.h"

/*
 * Apply a Bloom direct filter to every native access path of a base
 * relation.  Unlike a relation replacement, this preserves the shape of the
 * PathSet and lets PostgreSQL perform the final path comparison.
 */
extern bool bloompg_transform_filter_pathset(RelOptInfo *rel, Index rti,
	const BloomPGReplacement *replacement);
extern void bloompg_path_transform_reset(void);
extern char *bloompg_path_transform_json(void);
extern void bloompg_path_transform_record_replacement_parameterized(
	uint64 input_paths, uint64 retained_paths);

#endif
