/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The CPU's second-moment sum past 2^53 units for float_moment_metal, valid
 *  as Metal Shading Language (float_moment.metal) and as host C or C++
 *  (core/test/test_metal_float_moment_sum.c runs it against moment.c)
 *  (ADR-1497; ADR-1498 for the Metal twins).
 *
 *  moment.c::compute_2nd_moment() adds one float square per pixel into a
 *  double, in raster order. In units of 1 / scaler^2 every term is an integer
 *  below 2^32 and every step of the CPU's loop is the exact integer sum rounded
 *  to 53 significant bits (ties to even). Below 2^53 units nothing rounds and
 *  the kernels' exact uint64 sum is the CPU's sum; past it the order of the
 *  adds matters, and the twin forms the CPU's sum from rows with the four
 *  passes of ADR-1497: each row's exact sum, a plan per row from their prefix,
 *  each planned row's increments composed in pixel order, one checked walk
 *  with a run and term fallback. The plan is advice: the walk checks it at the
 *  exact sum, so a wrong plan costs time and never changes a result.
 *
 *  WHY THIS IS A COPY of feature/float_moment_sum.h and the parts of
 *  feature/ordered_sum.h it uses. Every pointer parameter of those headers
 *  needs a Metal address space (device, threadgroup, thread), and the CUDA,
 *  SYCL and HIP twins compile the shared text: a hook macro on each parameter
 *  would change their preprocessed output. The copy is statement for
 *  statement; core/test/test_metal_float_moment_exact_contract.py maps it back
 *  (rename rule below, address-space macros removed, comments and spacing
 *  ignored) and requires every function body to equal the shared one. A change
 *  to the shared header is a change to this one in the same PR.
 *
 *  Rename rule (shared -> this file):
 *    vmaf_moment_sum_*    -> vmaf_mtl_msum_*
 *    VMAF_MOMENT_SUM_*    -> VMAF_MTL_MSUM_*
 *    VMAF_MOMENT_PLAN_*   -> VMAF_MTL_MSUM_PLAN_*
 *    VMAF_MOMENT_WALK_*   -> VMAF_MTL_MSUM_WALK_*
 *    VMAF_MOMENT_SQUARE   -> VMAF_MTL_MSUM_SQUARE
 *    vmaf_ordsum_*        -> vmaf_mtl_os_*
 *    VmafOrdsumUnits      -> VmafMtlOrdsumUnits
 *    VMAF_ORDSUM_FUNC     -> VMAF_MTL_FUNC
 *    VMAF_ORDSUM_*        -> VMAF_MTL_OS_*
 *    half (a local)       -> half_way (`half` is a Metal type name)
 *  Address spaces: VMAF_MTL_DEV (device: the planes and the per-row arrays),
 *  VMAF_MTL_TG (threadgroup: staged batches, runs, the increment tree),
 *  VMAF_MTL_THR (thread: the running sum and its cursors); empty on the host.
 *
 *  Integers only (MSL has no double). The kernels that call these functions
 *  are in float_moment.metal: one wait per frame, in collect().
 */

#ifndef VMAF_FEATURE_METAL_METAL_FLOAT_MOMENT_SUM_H_
#define VMAF_FEATURE_METAL_METAL_FLOAT_MOMENT_SUM_H_

#include "metal_portable.h"
#include "metal_float_moment_math.h"

#if defined(__METAL_VERSION__)
#define VMAF_MTL_DEV device
#define VMAF_MTL_TG threadgroup
#define VMAF_MTL_THR thread
#else
#include <stddef.h>
#define VMAF_MTL_DEV
#define VMAF_MTL_TG
#define VMAF_MTL_THR
#endif

/* The term of a raw 16-bit sample: moment.c's float square. */
#define VMAF_MTL_MSUM_SQUARE(v) vmaf_mtl_moment_float_square(v)

/* A chunk increment at or above this is "does not fit the plan"; no plan binade
 * is named by this code. */
#define VMAF_MTL_OS_PLAN_TERMS (-32768)
#define VMAF_MTL_OS_UNFIT ((int64_t)1 << 54)

/* What a run of terms adds to a running sum that is `m * u`: `even` when m is
 * even, `odd` when it is odd. */
struct VmafMtlOrdsumUnits {
    int64_t even;
    int64_t odd;
};
#ifndef __cplusplus
typedef struct VmafMtlOrdsumUnits VmafMtlOrdsumUnits;
#endif

VMAF_MTL_FUNC VmafMtlOrdsumUnits vmaf_mtl_os_units(int64_t even, int64_t odd)
{
    VmafMtlOrdsumUnits r;
    r.even = even;
    r.odd = odd;
    return r;
}

/* `mantissa >> shift` rounded to nearest, 1 <= shift <= 53; a tie goes to the
 * value that makes the running integer even: m becomes m + whole or
 * m + whole + 1, whichever is even. */
VMAF_MTL_FUNC VmafMtlOrdsumUnits vmaf_mtl_os_round_shifted(uint64_t mantissa, int shift)
{
    const int64_t whole = (int64_t)(mantissa >> shift);
    const uint64_t rest = mantissa & (((uint64_t)1 << shift) - 1u);
    const uint64_t half_way = (uint64_t)1 << (shift - 1);
    if (rest != half_way) {
        const int64_t rounded = whole + (rest > half_way ? 1 : 0);
        return vmaf_mtl_os_units(rounded, rounded);
    }
    const int64_t odd_whole = whole & 1;
    return vmaf_mtl_os_units(whole + odd_whole, whole + 1 - odd_whole);
}

VMAF_MTL_FUNC int64_t vmaf_mtl_os_cap(int64_t v)
{
    return v > VMAF_MTL_OS_UNFIT ? VMAF_MTL_OS_UNFIT : v;
}

/* Increment of run `a` followed by run `b`. Associative, not commutative:
 * `b` starts at the parity `a` leaves. A result past VMAF_MTL_OS_UNFIT stays
 * there, so long runs cannot overflow. */
VMAF_MTL_FUNC VmafMtlOrdsumUnits vmaf_mtl_os_then(VmafMtlOrdsumUnits a, VmafMtlOrdsumUnits b)
{
    const int64_t even = a.even + ((a.even & 1) ? b.odd : b.even);
    const int64_t odd = a.odd + ((a.odd & 1) ? b.even : b.odd);
    return vmaf_mtl_os_units(vmaf_mtl_os_cap(even), vmaf_mtl_os_cap(odd));
}

/* The first sum at which an add can round: 2^53. */
#define VMAF_MTL_MSUM_EXACT_END ((uint64_t)1 << 53u)
/* Binades a sum of float squares can reach: [2^53, 2^63). */
#define VMAF_MTL_MSUM_MIN_BINADE 53
#define VMAF_MTL_MSUM_MAX_BINADE 62
/* Plan of a row that ends at or below 2^53: its terms add exactly. A binade
 * plan is in [VMAF_MTL_MSUM_MIN_BINADE, VMAF_MTL_MSUM_MAX_BINADE], and a
 * row the plan expects to cross a binade is VMAF_MTL_OS_PLAN_TERMS. */
#define VMAF_MTL_MSUM_PLAN_EXACT 0
/* Upper bound of vmaf_mtl_msum_shift(): a uint64_t has 64 bits. */
#define VMAF_MTL_MSUM_SHIFT_MAX 11u
/* Runs per row (one per lane of a work-group) and rows a walk stages at a
 * time. */
#define VMAF_MTL_MSUM_LANES 256u
#define VMAF_MTL_MSUM_BATCH 256u
/* The planes whose second moments the CPU's sum may round: ref, dis. */
#define VMAF_MTL_MSUM_PLANES 2u
/* What lane 0 of a walk asks its work-group to do next. */
#define VMAF_MTL_MSUM_WALK_LOAD 0u /* stage the batch of rows `operand` */
#define VMAF_MTL_MSUM_WALK_RUNS 1u /* compute the runs of row `operand` */
#define VMAF_MTL_MSUM_WALK_DONE 2u

/* 1 when a frame's sum of float squares can pass 2^53 units: every term is
 * below 2^(2 * bpc). Only such frames need anything beyond the exact integer
 * sum. */
VMAF_MTL_FUNC int vmaf_mtl_msum_may_round(unsigned w, unsigned h, unsigned bpc)
{
    if (bpc <= 8u || bpc > 16u)
        return 0;
    return (uint64_t)w * (uint64_t)h > (VMAF_MTL_MSUM_EXACT_END >> (2u * bpc));
}

/* The right shift that brings `x` below 2^53: 0 below 2^53, else the bit
 * length of x minus 53. */
VMAF_MTL_FUNC unsigned vmaf_mtl_msum_shift(uint64_t x)
{
    unsigned shift = 0u;
    for (unsigned i = 0u; i < VMAF_MTL_MSUM_SHIFT_MAX; i++) {
        if ((x >> shift) < VMAF_MTL_MSUM_EXACT_END)
            break;
        shift++;
    }
    return shift;
}

/* The binade [2^e, 2^(e+1)) of a sum at or above 2^53; VMAF_MTL_MSUM_PLAN_EXACT
 * below it. */
VMAF_MTL_FUNC int vmaf_mtl_msum_binade(uint64_t sum)
{
    if (sum < VMAF_MTL_MSUM_EXACT_END)
        return VMAF_MTL_MSUM_PLAN_EXACT;
    return 52 + (int)vmaf_mtl_msum_shift(sum);
}

/* 1 for a plan that names a binade. */
VMAF_MTL_FUNC int vmaf_mtl_msum_plan_is_binade(int plan)
{
    return plan >= VMAF_MTL_MSUM_MIN_BINADE && plan <= VMAF_MTL_MSUM_MAX_BINADE;
}

/* `sum + term` as the CPU's `cum += (double)term` forms it: the exact integer
 * sum rounded to 53 significant bits, to nearest, a tie to the even value
 * (the increment vmaf_mtl_os_round_shifted() gives an even start). */
VMAF_MTL_FUNC uint64_t vmaf_mtl_msum_add_term(uint64_t sum, uint32_t term)
{
    const uint64_t exact = sum + (uint64_t)term;
    const unsigned shift = vmaf_mtl_msum_shift(exact);
    if (shift == 0u)
        return exact;
    return (uint64_t)vmaf_mtl_os_round_shifted(exact, (int)shift).even << shift;
}

/* Plan of a row from the exact integer sums before and after it: the exact
 * range when it ends at or below 2^53, the binade it starts and ends in, or
 * VMAF_MTL_OS_PLAN_TERMS. The CPU's sum differs from the exact one past 2^53,
 * so this is advice that the walk checks. */
VMAF_MTL_FUNC int vmaf_mtl_msum_plan(uint64_t before, uint64_t after)
{
    if (after <= VMAF_MTL_MSUM_EXACT_END)
        return VMAF_MTL_MSUM_PLAN_EXACT;
    const int binade = vmaf_mtl_msum_binade(before);
    if (binade == VMAF_MTL_MSUM_PLAN_EXACT || binade != vmaf_mtl_msum_binade(after))
        return VMAF_MTL_OS_PLAN_TERMS;
    return binade;
}

/* Plans of `count` consecutive rows from their exact sums `totals`;
 * `*prefix` is the exact sum of every row before them and is advanced. */
VMAF_MTL_FUNC void vmaf_mtl_msum_plan_batch(VMAF_MTL_THR uint64_t *prefix,
                                            VMAF_MTL_TG const uint64_t *totals,
                                            VMAF_MTL_TG int *plans, unsigned count)
{
    uint64_t before = *prefix;
    for (unsigned i = 0; i < count && i < VMAF_MTL_MSUM_BATCH; i++) {
        const uint64_t after = before + totals[i];
        plans[i] = vmaf_mtl_msum_plan(before, after);
        before = after;
    }
    *prefix = before;
}

/* Increment of one term for a running sum in binade `plan`: the term in units
 * of 2^(plan - 52), rounded to nearest, a tie to the value that makes the
 * running integer even. Zero under a plan that is not a binade. */
VMAF_MTL_FUNC VmafMtlOrdsumUnits vmaf_mtl_msum_term_units(uint32_t term, int plan)
{
    if (!vmaf_mtl_msum_plan_is_binade(plan))
        return vmaf_mtl_os_units(0, 0);
    return vmaf_mtl_os_round_shifted((uint64_t)term, plan - 52);
}

/* The increments `units` (an even and an odd one per entry) of runs held in
 * lane order: at `step` (1, 2, 4, ...) every lane whose index is a multiple
 * of 2 * step takes the run `step` lanes above it, which finished the step
 * before. After the steps up to VMAF_MTL_MSUM_LANES / 2, entry 0 holds the
 * increment of all the runs in order. The composition is not commutative,
 * so the pairing must be adjacent. The caller separates the steps with a
 * barrier. */
VMAF_MTL_FUNC void vmaf_mtl_msum_tree_step(VMAF_MTL_TG int64_t *units, unsigned lane, unsigned step)
{
    if ((lane & (2u * step - 1u)) != 0u || lane + step >= VMAF_MTL_MSUM_LANES)
        return;
    const VmafMtlOrdsumUnits left =
        vmaf_mtl_os_units(units[(size_t)2u * lane], units[((size_t)2u * lane) + 1u]);
    const unsigned right_lane = lane + step;
    const VmafMtlOrdsumUnits right =
        vmaf_mtl_os_units(units[(size_t)2u * right_lane], units[((size_t)2u * right_lane) + 1u]);
    const VmafMtlOrdsumUnits both = vmaf_mtl_os_then(left, right);
    units[(size_t)2u * lane] = both.even;
    units[((size_t)2u * lane) + 1u] = both.odd;
}

/* Columns [*first, *end) of run `lane` of a row of `width` terms: the row cut
 * into VMAF_MTL_MSUM_LANES runs of equal length, the last ones shorter or
 * empty. */
VMAF_MTL_FUNC void vmaf_mtl_msum_run_bounds(unsigned width, unsigned lane,
                                            VMAF_MTL_THR unsigned *first,
                                            VMAF_MTL_THR unsigned *end)
{
    const unsigned len = (width + VMAF_MTL_MSUM_LANES - 1u) / VMAF_MTL_MSUM_LANES;
    const unsigned lo = lane * len;
    *first = lo < width ? lo : width;
    *end = lo + len < width ? lo + len : width;
}

/* Adds a run of terms (a row, or a part of one) to the CPU's running sum
 * `*sum`. `total` is the exact integer sum of the run's terms and `units`
 * their increments under `plan`, composed in pixel order. The run is added
 * exactly when the sum stays at or below 2^53, and from `units` when the sum
 * is in binade `plan` and the run does not take it past the binade's end.
 * Returns 1 when the run is added, 0 when the caller must add its terms one
 * by one (`*sum` is unchanged then). */
VMAF_MTL_FUNC int vmaf_mtl_msum_add_run(VMAF_MTL_THR uint64_t *sum, int plan, uint64_t total,
                                        VmafMtlOrdsumUnits units)
{
    const uint64_t s = *sum;
    if (s <= VMAF_MTL_MSUM_EXACT_END && total <= VMAF_MTL_MSUM_EXACT_END - s) {
        *sum = s + total;
        return 1;
    }
    if (!vmaf_mtl_msum_plan_is_binade(plan) || vmaf_mtl_msum_binade(s) != plan)
        return 0;
    /* s is a multiple of 2^shift, m in [2^52, 2^53), and the run's increments
     * are rounded for that grid (vmaf_mtl_os_add_chunk_bits() on the bits of
     * an integer-valued sum). */
    const unsigned shift = (unsigned)(plan - 52);
    const uint64_t m = s >> shift;
    const uint64_t end = m + (uint64_t)((m & 1u) ? units.odd : units.even);
    /* The run may end on the binade's end, 2^53 multiples, and no further. */
    if (end > VMAF_MTL_MSUM_EXACT_END)
        return 0;
    *sum = end << shift;
    return 1;
}

/* Adds staged rows to the CPU's running sum `*sum`, from row `*row` up to
 * `end`; row r is staged at index r - first (its plan, its exact sum and its
 * increments as an even and an odd entry). Returns 0 when every row up to
 * `end` is added, 1 when row `*row` must be added as runs. */
VMAF_MTL_FUNC int vmaf_mtl_msum_walk_rows(VMAF_MTL_THR uint64_t *sum, VMAF_MTL_THR unsigned *row,
                                          unsigned first, unsigned end,
                                          VMAF_MTL_TG const int *plans,
                                          VMAF_MTL_TG const uint64_t *totals,
                                          VMAF_MTL_TG const int64_t *units)
{
    unsigned r = *row;
    int stopped = 0;
    for (unsigned n = 0; n < VMAF_MTL_MSUM_BATCH && r < end; n++) {
        const unsigned i = r - first;
        const VmafMtlOrdsumUnits u =
            vmaf_mtl_os_units(units[(size_t)2u * i], units[((size_t)2u * i) + 1u]);
        if (!vmaf_mtl_msum_add_run(sum, plans[i], totals[i], u)) {
            stopped = 1;
            break;
        }
        r++;
    }
    *row = r;
    return stopped;
}

/* The two binades the runs of a row keep increments for, from the sum the row
 * starts at: the binade it is in (VMAF_MTL_MSUM_PLAN_EXACT below 2^53) and the
 * next one. A row crosses at most one binade. */
VMAF_MTL_FUNC void vmaf_mtl_msum_run_binades(uint64_t sum, VMAF_MTL_THR int *low,
                                             VMAF_MTL_THR int *high)
{
    const int binade = vmaf_mtl_msum_binade(sum);
    *low = binade;
    *high = binade == VMAF_MTL_MSUM_PLAN_EXACT ? VMAF_MTL_MSUM_MIN_BINADE : binade + 1;
}

/* Adds the runs of a row to `*sum`, from run `*run` up to `count`; run i has
 * the exact sum totals[i] and the increments units_low / units_high (even
 * and odd entries) under the binades `low` and `high` of
 * vmaf_mtl_msum_run_binades(). Returns 0 when every run is added, 1 when
 * run `*run` must be added term by term. */
VMAF_MTL_FUNC int vmaf_mtl_msum_walk_runs(VMAF_MTL_THR uint64_t *sum, VMAF_MTL_THR unsigned *run,
                                          unsigned count, int low, int high,
                                          VMAF_MTL_TG const uint64_t *totals,
                                          VMAF_MTL_TG const int64_t *units_low,
                                          VMAF_MTL_TG const int64_t *units_high)
{
    unsigned i = *run;
    int stopped = 0;
    for (unsigned n = 0; n < VMAF_MTL_MSUM_LANES && i < count; n++) {
        const int binade = vmaf_mtl_msum_binade(*sum);
        VMAF_MTL_TG const int64_t *units = binade == low ? units_low : units_high;
        /* Increments exist for the two binades only: under any other the
         * run is added term by term. */
        const int plan = (binade == low || binade == high) ? binade : VMAF_MTL_OS_PLAN_TERMS;
        const VmafMtlOrdsumUnits u =
            vmaf_mtl_os_units(units[(size_t)2u * i], units[((size_t)2u * i) + 1u]);
        if (!vmaf_mtl_msum_add_run(sum, plan, totals[i], u)) {
            stopped = 1;
            break;
        }
        i++;
    }
    *run = i;
    return stopped;
}

/* Row `row` of a plane that starts at `luma`, `stride` bytes per row. */
VMAF_MTL_FUNC VMAF_MTL_DEV const uint16_t *vmaf_mtl_msum_line(VMAF_MTL_DEV const uint8_t *luma,
                                                              size_t stride, unsigned row)
{
    return (VMAF_MTL_DEV const uint16_t *)(luma + ((size_t)row * stride));
}

/* Kernel 1, one lane: the exact sum of the terms of columns lane, lane +
 * VMAF_MTL_MSUM_LANES, ... of a row. */
VMAF_MTL_FUNC uint64_t vmaf_mtl_msum_lane_total(VMAF_MTL_DEV const uint16_t *line, unsigned width,
                                                unsigned lane)
{
    uint64_t total = 0u;
    for (unsigned x = lane; x < width; x += VMAF_MTL_MSUM_LANES)
        total += (uint64_t)VMAF_MTL_MSUM_SQUARE(line[x]);
    return total;
}

/* The exact sum and the increments under `plan` of columns [first, end) of a
 * row, composed in pixel order. */
VMAF_MTL_FUNC uint64_t vmaf_mtl_msum_run(VMAF_MTL_DEV const uint16_t *line, unsigned first,
                                         unsigned end, int plan,
                                         VMAF_MTL_THR VmafMtlOrdsumUnits *units)
{
    uint64_t total = 0u;
    VmafMtlOrdsumUnits u = vmaf_mtl_os_units(0, 0);
    for (unsigned x = first; x < end; x++) {
        const uint32_t term = (uint32_t)VMAF_MTL_MSUM_SQUARE(line[x]);
        total += term;
        u = vmaf_mtl_os_then(u, vmaf_mtl_msum_term_units(term, plan));
    }
    *units = u;
    return total;
}

/* Kernel 3, one lane: the increments of its run of a row under `plan`, into
 * entries 2 * lane and 2 * lane + 1 of `units`. */
VMAF_MTL_FUNC void vmaf_mtl_msum_lane_units(VMAF_MTL_DEV const uint16_t *line, unsigned width,
                                            int plan, unsigned lane, VMAF_MTL_TG int64_t *units)
{
    unsigned first;
    unsigned end;
    vmaf_mtl_msum_run_bounds(width, lane, &first, &end);
    VmafMtlOrdsumUnits u;
    (void)vmaf_mtl_msum_run(line, first, end, plan, &u);
    units[(size_t)2u * lane] = u.even;
    units[((size_t)2u * lane) + 1u] = u.odd;
}

/* Kernel 4, one lane: stages row `first + lane` of a plane's arrays (`rows`
 * rows) into entry `lane` of the batch. */
VMAF_MTL_FUNC void vmaf_mtl_msum_walk_stage(VMAF_MTL_DEV const int *row_plans,
                                            VMAF_MTL_DEV const uint64_t *row_totals,
                                            VMAF_MTL_DEV const int64_t *row_units, unsigned rows,
                                            unsigned first, unsigned lane, VMAF_MTL_TG int *plans,
                                            VMAF_MTL_TG uint64_t *totals,
                                            VMAF_MTL_TG int64_t *units)
{
    const unsigned row = first + lane;
    if (row >= rows)
        return;
    plans[lane] = row_plans[row];
    totals[lane] = row_totals[row];
    units[(size_t)2u * lane] = row_units[(size_t)2u * row];
    units[((size_t)2u * lane) + 1u] = row_units[((size_t)2u * row) + 1u];
}

/* Kernel 4, one lane, for a row added as runs: its run's exact sum and its
 * increments under the two binades of vmaf_mtl_msum_run_binades() for the
 * sum `start` the row starts at. */
VMAF_MTL_FUNC void vmaf_mtl_msum_walk_run(VMAF_MTL_DEV const uint16_t *line, unsigned width,
                                          uint64_t start, unsigned lane,
                                          VMAF_MTL_TG uint64_t *totals,
                                          VMAF_MTL_TG int64_t *low_units,
                                          VMAF_MTL_TG int64_t *high_units)
{
    int low;
    int high;
    vmaf_mtl_msum_run_binades(start, &low, &high);
    unsigned first;
    unsigned end;
    vmaf_mtl_msum_run_bounds(width, lane, &first, &end);
    VmafMtlOrdsumUnits u;
    totals[lane] = vmaf_mtl_msum_run(line, first, end, low, &u);
    low_units[(size_t)2u * lane] = u.even;
    low_units[((size_t)2u * lane) + 1u] = u.odd;
    (void)vmaf_mtl_msum_run(line, first, end, high, &u);
    high_units[(size_t)2u * lane] = u.even;
    high_units[((size_t)2u * lane) + 1u] = u.odd;
}

/* Kernel 4, lane 0: adds a row to `sum` as its runs, from what every lane
 * staged with vmaf_mtl_msum_walk_run(), and a run that crosses a binade
 * term by term, in pixel order. */
VMAF_MTL_FUNC uint64_t vmaf_mtl_msum_walk_row_runs(VMAF_MTL_DEV const uint16_t *line,
                                                   unsigned width, uint64_t sum,
                                                   VMAF_MTL_TG const uint64_t *totals,
                                                   VMAF_MTL_TG const int64_t *low_units,
                                                   VMAF_MTL_TG const int64_t *high_units)
{
    int low;
    int high;
    vmaf_mtl_msum_run_binades(sum, &low, &high);
    unsigned run = 0u;
    for (unsigned n = 0; n < VMAF_MTL_MSUM_LANES; n++) {
        if (!vmaf_mtl_msum_walk_runs(&sum, &run, VMAF_MTL_MSUM_LANES, low, high, totals, low_units,
                                     high_units))
            break;
        unsigned first;
        unsigned end;
        vmaf_mtl_msum_run_bounds(width, run, &first, &end);
        for (unsigned x = first; x < end; x++)
            sum = vmaf_mtl_msum_add_term(sum, (uint32_t)VMAF_MTL_MSUM_SQUARE(line[x]));
        run++;
    }
    return sum;
}

/* Kernel 4, lane 0: walks the staged batch that starts at row `first` from
 * row `*row`, and returns what the work-group does next
 * (VMAF_MTL_MSUM_WALK_*) with its operand. */
VMAF_MTL_FUNC unsigned
vmaf_mtl_msum_walk_next(unsigned rows, unsigned first, VMAF_MTL_THR uint64_t *sum,
                        VMAF_MTL_THR unsigned *row, VMAF_MTL_TG const int *plans,
                        VMAF_MTL_TG const uint64_t *totals, VMAF_MTL_TG const int64_t *units,
                        VMAF_MTL_THR unsigned *operand)
{
    const unsigned end = rows - first < VMAF_MTL_MSUM_BATCH ? rows : first + VMAF_MTL_MSUM_BATCH;
    if (*row < end && vmaf_mtl_msum_walk_rows(sum, row, first, end, plans, totals, units)) {
        *operand = *row;
        return VMAF_MTL_MSUM_WALK_RUNS;
    }
    *operand = *row / VMAF_MTL_MSUM_BATCH;
    return *row < rows ? VMAF_MTL_MSUM_WALK_LOAD : VMAF_MTL_MSUM_WALK_DONE;
}
#endif /* VMAF_FEATURE_METAL_METAL_FLOAT_MOMENT_SUM_H_ */
