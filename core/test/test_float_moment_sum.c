/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * feature/float_moment_sum.h against the CPU's own second moment
 * (ADR-1497). No device is needed.
 *
 * moment.c::compute_2nd_moment() adds one float square per pixel into a
 * double, in raster order. Past 2^53 units of 1 / scaler^2 that sum rounds,
 * and the float_moment twins form it from rows with the header's helpers:
 * exact row sums, a plan per row from their prefix, the rows' increments
 * composed in pixel order, and a walk that adds a row from its increment or
 * cuts it into runs and adds a run that crosses a binade term by term. This
 * test runs that pipeline on the host, laid out as the kernels lay it out
 * (VMAF_MOMENT_SUM_LANES lanes, an ordered tree over them, batches of
 * VMAF_MOMENT_SUM_BATCH rows), and compares the second moment it gives with
 * the CPU's bit for bit on the frames of float_moment_twin_parity.h past
 * 2^53 (each fails against the exact sum where the CPU rounds) and on
 * larger ones up to 7680x4320, which crosses four binades. It runs the walk
 * again with plans that are deliberately wrong, which must change nothing,
 * and checks the one-term rule the rest is built on against the double add.
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

/* The lanes' steps of the kernels, with the CPU's term of a sample. */
#define VMAF_MOMENT_SQUARE(v) float_moment_twin_float_square(v)
#include "feature/float_moment_sum.h"

#include "float_moment_sum_model.h"
