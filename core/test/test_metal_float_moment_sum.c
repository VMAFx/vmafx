/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * feature/metal/metal_float_moment_sum.h against the CPU's own second moment
 * (ADR-1497, ADR-1498). No Apple device is needed.
 *
 * The header is the Metal copy of feature/float_moment_sum.h: the same
 * statements with a Metal address space on each pointer, which
 * test_metal_float_moment_exact_contract.py holds to the shared text. It is
 * what float_moment.metal's five kernels call on a 16-bit frame that can pass
 * 2^53 units of 1 / scaler^2, where the CPU's running double rounds as it
 * adds. This test runs the pipeline on the host laid out as those kernels lay
 * it out (VMAF_MTL_MSUM_LANES lanes, an ordered tree over them, batches of
 * VMAF_MTL_MSUM_BATCH rows) and compares the second moment it gives with
 * picture_copy() + compute_2nd_moment() bit for bit: on the frames of
 * float_moment_twin_parity.h past 2^53 (each fails against the exact sum where
 * the CPU rounds), on 3840x2160 noise and bright frames, on a frame past 2^54
 * and one up to 7680x4320 (four binades), and again with plans that are
 * deliberately wrong, which must change nothing. The model, the cases and the
 * one-term rule are those of test_float_moment_sum.c (float_moment_sum_model.h).
 * What only a device shows: the kernels' threadgroup barriers and address
 * spaces; test_metal_float_moment_parity runs there.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/moment.h"
#include "feature/picture_copy.h"
#include "float_moment_twin_parity.h"

#include "feature/metal/metal_float_moment_sum.h"

/* The shared names of the model, spelled as the Metal copy spells them (the
 * rename rule at the top of metal_float_moment_sum.h). */
#define VMAF_MOMENT_SUM_LANES VMAF_MTL_MSUM_LANES
#define VMAF_MOMENT_SUM_BATCH VMAF_MTL_MSUM_BATCH
#define VMAF_MOMENT_PLAN_EXACT VMAF_MTL_MSUM_PLAN_EXACT
#define VMAF_MOMENT_WALK_LOAD VMAF_MTL_MSUM_WALK_LOAD
#define VMAF_MOMENT_WALK_RUNS VMAF_MTL_MSUM_WALK_RUNS
#define VMAF_MOMENT_WALK_DONE VMAF_MTL_MSUM_WALK_DONE
#define VMAF_ORDSUM_PLAN_TERMS VMAF_MTL_OS_PLAN_TERMS
#define vmaf_moment_sum_line vmaf_mtl_msum_line
#define vmaf_moment_sum_lane_total vmaf_mtl_msum_lane_total
#define vmaf_moment_sum_plan_batch vmaf_mtl_msum_plan_batch
#define vmaf_moment_sum_plan_is_binade vmaf_mtl_msum_plan_is_binade
#define vmaf_moment_sum_lane_units vmaf_mtl_msum_lane_units
#define vmaf_moment_sum_tree_step vmaf_mtl_msum_tree_step
#define vmaf_moment_sum_walk_stage vmaf_mtl_msum_walk_stage
#define vmaf_moment_sum_walk_run vmaf_mtl_msum_walk_run
#define vmaf_moment_sum_walk_row_runs vmaf_mtl_msum_walk_row_runs
#define vmaf_moment_sum_walk_next vmaf_mtl_msum_walk_next
#define vmaf_moment_sum_add_term vmaf_mtl_msum_add_term
#define vmaf_moment_sum_may_round vmaf_mtl_msum_may_round

#include "float_moment_sum_model.h"
