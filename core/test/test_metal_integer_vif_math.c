/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The border index of the integer VIF Metal kernels against the CPU's
 * reflection, without a device (T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29,
 * ADR-1498). core/src/feature/metal/metal_integer_vif_math.h is the same code
 * the kernels compile (integer_vif.metal's vif_mirror()).
 *
 * integer_vif.c reflects each filter tap once about the edge sample
 * (pad_top_and_bottom, PADDING_SQ_DATA: idx < 0 -> -idx, idx >= sup ->
 * 2 * (sup - 1) - idx); cpu_reflect() below is that rule. The cases:
 *   - the fold equals the single reflection for every index it brings into
 *     the plane, on every plane length up to 4096;
 *   - from the 16-pixel minimum on, every tap an output of a compute kernel
 *     (scale filter) or of a decimation kernel (next scale's filter, at
 *     2 * x) reads is such an index, so the twin reads the CPU's samples;
 *   - every tile sample the compute kernels load, read or not, lands inside
 *     the plane (a single reflection sent some outside the buffer);
 *   - the smallest size from which every read tap is a single reflection is
 *     16, the vif_metal_min_dim() of integer_vif_metal.mm, from the CPU's
 *     filter widths (integer_vif.h).
 */

#include <stdbool.h>
#include <stdint.h>

#include "test.h"

#include "feature/integer_vif.h"
#include "feature/metal/metal_integer_vif_math.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define SCALES 4
#define BLOCK 16
#define MAX_SUP 4096
#define MAX_DIM 300
#define MIN_DIM 16

/* integer_vif.c's border: one reflection about the edge sample. */
static int cpu_reflect(int idx, int sup)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= sup) {
        return (2 * (sup - 1)) - idx;
    }
    return idx;
}

static bool reflects_into(int idx, int sup)
{
    const int r = cpu_reflect(idx, sup);
    return r >= 0 && r < sup;
}

/* Samples of scale s of a frame dimension, as integer_vif_metal.mm halves it. */
static int scale_len(int dim, int scale)
{
    return dim >> scale;
}

static int half_width(int scale)
{
    return vif_filter1d_width[scale] / 2;
}

static char *test_fold_is_the_cpu_reflection(void)
{
    for (int sup = 2; sup <= MAX_SUP; sup++) {
        for (int idx = -(sup - 1); idx <= 2 * (sup - 1); idx++) {
            mu_assert("the fold differs from the CPU's reflection",
                      vmaf_mtl_vif_mirror(idx, sup) == cpu_reflect(idx, sup));
        }
    }
    mu_assert("a one-sample plane reads its sample", vmaf_mtl_vif_mirror(-3, 1) == 0);
    return NULL;
}

/* Every tap an output of scale `scale` reads, compute and decimation, is one
 * reflection away from the plane. */
static bool reads_are_single_reflections(int dim, int scale)
{
    const int sup = scale_len(dim, scale);
    const int hfw = half_width(scale);
    for (int x = 0; x < sup; x++) {
        if (!reflects_into(x - hfw, sup) || !reflects_into(x + hfw, sup)) {
            return false;
        }
    }
    if (scale + 1 >= SCALES) {
        return true;
    }
    const int out = scale_len(dim, scale + 1);
    const int rd_hfw = half_width(scale + 1);
    for (int x = 0; x < out; x++) {
        if (!reflects_into((2 * x) - rd_hfw, sup) || !reflects_into((2 * x) + rd_hfw, sup)) {
            return false;
        }
    }
    return true;
}

static bool every_scale_reads_single_reflections(int dim)
{
    for (int scale = 0; scale < SCALES; scale++) {
        if (!reads_are_single_reflections(dim, scale)) {
            return false;
        }
    }
    return true;
}

/* Every sample of every compute tile at scale `scale` lands in the plane. */
static bool tile_loads_in_plane(int dim, int scale)
{
    const int sup = scale_len(dim, scale);
    const int hfw = half_width(scale);
    const int groups = (sup + BLOCK - 1) / BLOCK;
    for (int g = 0; g < groups; g++) {
        for (int t = 0; t < BLOCK + (2 * hfw); t++) {
            const int at = vmaf_mtl_vif_mirror((g * BLOCK) - hfw + t, sup);
            if (at < 0 || at >= sup) {
                return false;
            }
        }
    }
    return true;
}

static char *test_reads_from_the_minimum_are_the_cpus(void)
{
    for (int dim = MIN_DIM; dim <= MAX_DIM; dim++) {
        mu_assert("a read tap is not the CPU's sample at or above the minimum",
                  every_scale_reads_single_reflections(dim));
    }
    mu_assert("853 wide: a read tap is not the CPU's sample",
              every_scale_reads_single_reflections(853));
    mu_assert("4096 wide: a read tap is not the CPU's sample",
              every_scale_reads_single_reflections(4096));
    return NULL;
}

static char *test_tile_loads_stay_in_the_plane(void)
{
    bool single_reflection_escapes = false;
    for (int dim = MIN_DIM; dim <= MAX_DIM; dim++) {
        for (int scale = 0; scale < SCALES; scale++) {
            mu_assert("a tile load leaves the plane", tile_loads_in_plane(dim, scale));
            const int sup = scale_len(dim, scale);
            const int last = BLOCK + half_width(scale) - 1;
            single_reflection_escapes |= !reflects_into(last, sup);
        }
    }
    /* What the fold fixes: 16x16 at scale 1 (8 samples, halo 4) loads index
     * 19, which one reflection sends to -5. */
    mu_assert("one reflection keeps every tile load in the plane", single_reflection_escapes);
    mu_assert("16 wide, scale 1: index 19 is sent outside by one reflection",
              cpu_reflect(19, 8) == -5 && vmaf_mtl_vif_mirror(19, 8) == 5);
    return NULL;
}

/* The minimum of integer_vif_metal.mm, from the CPU's filter widths: below it
 * some read tap needs a second reflection, from it on none does. */
static char *test_minimum_is_sixteen(void)
{
    int smallest = MAX_DIM + 1;
    for (int dim = MAX_DIM; dim >= 1; dim--) {
        if (!every_scale_reads_single_reflections(dim)) {
            break;
        }
        smallest = dim;
    }
    mu_assert("the smallest size whose reads are all single reflections is not 16",
              smallest == MIN_DIM);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_fold_is_the_cpu_reflection);
    mu_run_test(test_reads_from_the_minimum_are_the_cpus);
    mu_run_test(test_tile_loads_stay_in_the_plane);
    mu_run_test(test_minimum_is_sixteen);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
