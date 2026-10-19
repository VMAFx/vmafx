/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The host model of the float_moment twins' rounded second-moment sum
 * (ADR-1497) and its cases, for a test to instantiate against one header of
 * the arithmetic: test_float_moment_sum.c includes it after
 * feature/float_moment_sum.h (the CUDA, SYCL and HIP twins), and
 * test_metal_float_moment_sum.c after metal/metal_float_moment_sum.h, with the
 * shared names defined to the Metal copy's (ADR-1498). The includer defines,
 * before this header: the term `VMAF_MOMENT_SQUARE`'s function
 * float_moment_twin_float_square() through its header, the functions
 * vmaf_moment_sum_* and the constants VMAF_MOMENT_SUM_LANES /
 * VMAF_MOMENT_SUM_BATCH / VMAF_MOMENT_PLAN_EXACT / VMAF_MOMENT_WALK_* /
 * VMAF_ORDSUM_PLAN_TERMS the model calls. The model runs the pipeline laid
 * out as the kernels lay it out and compares with picture_copy() +
 * compute_2nd_moment(); see test_float_moment_sum.c for the cases.
 */

#ifndef LIBVMAF_TEST_FLOAT_MOMENT_SUM_MODEL_H_
#define LIBVMAF_TEST_FLOAT_MOMENT_SUM_MODEL_H_
#include "float_bits.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define RANDOM_PAIRS 400000u
#define WRONG_PLAN_KINDS 4u

/* The rows of one 16-bit luma plane and what the kernels keep per row. */
typedef struct MomentModel {
    unsigned w;
    unsigned h;
    const uint8_t *luma;
    size_t stride; /* bytes */
    uint64_t *totals;
    int *plans;
    int64_t *units; /* even, odd per row */
} MomentModel;

static const uint16_t *model_line(const MomentModel *m, unsigned row)
{
    return vmaf_moment_sum_line(m->luma, m->stride, row);
}

/* Kernel 1: the exact sum of every row, the lanes' sums added. */
static void model_row_totals(MomentModel *m)
{
    for (unsigned row = 0; row < m->h; row++) {
        uint64_t total = 0u;
        for (unsigned lane = 0; lane < VMAF_MOMENT_SUM_LANES; lane++) {
            total += vmaf_moment_sum_lane_total(model_line(m, row), m->w, lane);
        }
        m->totals[row] = total;
    }
}

/* Kernel 2: the plans, a batch at a time; `wrong` replaces some of them with
 * a binade too low or too high, "exact" or "terms" (0: the real plans). */
static void model_plans(MomentModel *m, unsigned wrong)
{
    uint64_t prefix = 0u;
    for (unsigned first = 0; first < m->h; first += VMAF_MOMENT_SUM_BATCH) {
        const unsigned count =
            m->h - first < VMAF_MOMENT_SUM_BATCH ? m->h - first : VMAF_MOMENT_SUM_BATCH;
        vmaf_moment_sum_plan_batch(&prefix, m->totals + first, m->plans + first, count);
    }
    for (unsigned row = 0; row < m->h && wrong != 0u; row++) {
        const uint32_t pick = float_moment_twin_hash(row, wrong, 77u) % 6u;
        const int plan = m->plans[row];
        const int binade = vmaf_moment_sum_plan_is_binade(plan) ? plan : 54;
        const int replaced[4] = {binade - 1, binade + 1, VMAF_MOMENT_PLAN_EXACT,
                                 VMAF_ORDSUM_PLAN_TERMS};
        m->plans[row] = pick < 4u ? replaced[pick] : plan;
    }
}

/* Kernel 3: every planned row's increments under its plan, one run per lane
 * and the lanes composed in an ordered tree. */
static void model_row_units(MomentModel *m)
{
    static int64_t lanes[2u * VMAF_MOMENT_SUM_LANES];
    for (unsigned row = 0; row < m->h; row++) {
        const int plan = m->plans[row];
        lanes[0] = 0;
        lanes[1] = 0;
        for (unsigned lane = 0;
             lane < VMAF_MOMENT_SUM_LANES && vmaf_moment_sum_plan_is_binade(plan); lane++) {
            vmaf_moment_sum_lane_units(model_line(m, row), m->w, plan, lane, lanes);
        }
        for (unsigned step = 1u; step < VMAF_MOMENT_SUM_LANES; step <<= 1u) {
            for (unsigned lane = 0; lane < VMAF_MOMENT_SUM_LANES; lane++) {
                vmaf_moment_sum_tree_step(lanes, lane, step);
            }
        }
        m->units[(size_t)2u * row] = lanes[0];
        m->units[((size_t)2u * row) + 1u] = lanes[1];
    }
}

/* Kernel 4's shared memory. */
typedef struct WalkShared {
    int plans[VMAF_MOMENT_SUM_BATCH];
    uint64_t totals[VMAF_MOMENT_SUM_BATCH];
    int64_t units[2u * VMAF_MOMENT_SUM_BATCH];
    uint64_t run_totals[VMAF_MOMENT_SUM_LANES];
    int64_t run_low[2u * VMAF_MOMENT_SUM_LANES];
    int64_t run_high[2u * VMAF_MOMENT_SUM_LANES];
} WalkShared;

/* Kernel 4, every lane's step of one round. */
static void model_walk_lanes(const MomentModel *m, unsigned todo, unsigned what, uint64_t walked,
                             WalkShared *sh)
{
    for (unsigned lane = 0; lane < VMAF_MOMENT_SUM_LANES; lane++) {
        if (todo == VMAF_MOMENT_WALK_LOAD) {
            vmaf_moment_sum_walk_stage(m->plans, m->totals, m->units, m->h,
                                       what * VMAF_MOMENT_SUM_BATCH, lane, sh->plans, sh->totals,
                                       sh->units);
        } else {
            vmaf_moment_sum_walk_run(model_line(m, what), m->w, walked, lane, sh->run_totals,
                                     sh->run_low, sh->run_high);
        }
    }
}

/* Kernel 4: the walk over the rows, round by round as the kernels run it. */
static uint64_t model_walk(const MomentModel *m)
{
    static WalkShared sh;
    unsigned command = VMAF_MOMENT_WALK_LOAD;
    unsigned operand = 0u;
    uint64_t sum = 0u;
    unsigned row = 0u;
    unsigned first = 0u;
    const unsigned rounds = m->h + m->h / VMAF_MOMENT_SUM_BATCH + 2u;
    for (unsigned round = 0; round < rounds && command != VMAF_MOMENT_WALK_DONE; round++) {
        const unsigned what = operand;
        model_walk_lanes(m, command, what, sum, &sh);
        if (command == VMAF_MOMENT_WALK_RUNS) {
            sum = vmaf_moment_sum_walk_row_runs(model_line(m, what), m->w, sum, sh.run_totals,
                                                sh.run_low, sh.run_high);
            row = what + 1u;
        } else {
            first = what * VMAF_MOMENT_SUM_BATCH;
        }
        command = vmaf_moment_sum_walk_next(m->h, first, &sum, &row, sh.plans, sh.totals, sh.units,
                                            &operand);
    }
    return command == VMAF_MOMENT_WALK_DONE ? sum : 0u;
}

/* The sum the twins form for luma `pic`, with the real plans (`wrong` 0) or
 * deliberately wrong ones. */
static uint64_t model_sum(const VmafPicture *pic, unsigned wrong, MomentModel *m)
{
    m->w = pic->w[0];
    m->h = pic->h[0];
    m->luma = (const uint8_t *)pic->data[0];
    m->stride = (size_t)pic->stride[0];
    model_row_totals(m);
    model_plans(m, wrong);
    model_row_units(m);
    return model_walk(m);
}

/* The exact integer sum of the rows (what the twins returned before ADR-1497). */
static uint64_t model_exact(const MomentModel *m)
{
    uint64_t exact = 0u;
    for (unsigned row = 0; row < m->h; row++) {
        exact += m->totals[row];
    }
    return exact;
}

/* The CPU's second moment of luma `pic`: picture_copy(), then
 * compute_2nd_moment(), as float_moment.c does. */
static int cpu_moment(VmafPicture *pic, double *moment)
{
    const size_t stride = (size_t)pic->w[0] * sizeof(float);
    float *buf = malloc(stride * pic->h[0]);
    if (!buf) {
        return -1;
    }
    picture_copy(buf, (ptrdiff_t)stride, pic, 0, pic->bpc, 0);
    const int err = compute_2nd_moment(buf, (int)pic->w[0], (int)pic->h[0], (int)stride, moment);
    free(buf);
    return err;
}

static double moment_of(uint64_t sum, const VmafPicture *pic)
{
    return ((double)sum / 65536.0) / ((double)pic->w[0] * (double)pic->h[0]);
}

/* Mismatches of the model against the CPU on one case: the real plan and
 * every kind of wrong plan. A case that says the CPU rounds must also differ
 * from the exact sum. */
static unsigned case_mismatches(const FloatMomentTwinCase *c, MomentModel *m)
{
    VmafPicture pic;
    double cpu = 0.0;
    if (float_moment_twin_fill(&pic, c, 1u) || cpu_moment(&pic, &cpu)) {
        return 1u;
    }
    unsigned bad = 0u;
    uint64_t exact = 0u;
    for (unsigned wrong = 0; wrong <= WRONG_PLAN_KINDS; wrong++) {
        const uint64_t sum = model_sum(&pic, wrong, m);
        exact = wrong == 0u ? model_exact(m) : exact;
        if (!vmaf_test_identical_f64(moment_of(sum, &pic), cpu)) {
            bad++;
            (void)fprintf(stderr, "\n%s (wrong plans %u): model %.17g, CPU %.17g", c->name, wrong,
                          moment_of(sum, &pic), cpu);
        }
    }
    if ((!vmaf_test_identical_f64(moment_of(exact, &pic), cpu)) != c->rounds) {
        bad++;
        (void)fprintf(stderr, "\n%s: the exact sum %s the CPU's", c->name,
                      c->rounds ? "equals" : "differs from");
    }
    (void)vmaf_picture_unref(&pic);
    return bad;
}

static unsigned model_cases_mismatches(const FloatMomentTwinCase *cases, size_t count)
{
    const size_t rows = 4320u;
    MomentModel m = {0};
    m.totals = calloc(rows, sizeof(uint64_t));
    m.plans = calloc(rows, sizeof(int));
    m.units = calloc(2u * rows, sizeof(int64_t));
    unsigned bad = (!m.totals || !m.plans || !m.units) ? 1u : 0u;
    for (size_t k = 0; k < count && bad == 0u; k++) {
        bad += case_mismatches(&cases[k], &m);
    }
    free(m.totals);
    free(m.plans);
    free(m.units);
    return bad;
}

static char *test_model_equals_cpu_past_2_53(void)
{
    mu_assert("the model is not the CPU's sum on the parity cases",
              model_cases_mismatches(FLOAT_MOMENT_TWIN_PAST_CASES,
                                     FLOAT_MOMENT_TWIN_PAST_CASE_COUNT) == 0u);
    return NULL;
}

static char *test_model_equals_cpu_large(void)
{
    static const FloatMomentTwinCase cases[] = {
        {"16-bit 3840x2160 noise", 3840u, 2160u, 16u, 0u, 65535u, 0u, 0u, 0u, true},
        {"16-bit 3840x2160 near the peak", 3840u, 2160u, 16u, 65528u, 65535u, 0u, 0u, 0u, false},
        {"16-bit sum 2^54 and seven ones", 4096u, 4320u, 16u, 0u, 0u, 0u, (uint64_t)1 << 54, 7u,
         true},
        {"16-bit 7680x4320 past 2^56", 7680u, 4320u, 16u, 60000u, 65535u, 10u, 0u, 0u, true},
    };
    mu_assert("the model is not the CPU's sum on the large cases",
              model_cases_mismatches(cases, sizeof(cases) / sizeof(cases[0])) == 0u);
    return NULL;
}

/* The one-term rule: vmaf_moment_sum_add_term() is the double add of a sum
 * the CPU can hold and a term below 2^32. */
static bool add_term_is_double_add(uint64_t sum, uint32_t term)
{
    const double expected = (double)sum + (double)term;
    return vmaf_moment_sum_add_term(sum, term) == (uint64_t)expected;
}

static char *test_add_term_is_the_double_add(void)
{
    static const uint64_t sums[] = {
        0u,
        ((uint64_t)1 << 53) - 3u,
        ((uint64_t)1 << 53) - 1u,
        (uint64_t)1 << 53,
        ((uint64_t)1 << 53) + 2u,
        ((uint64_t)1 << 54) - 2u,
        ((uint64_t)1 << 54) - 4u,
        (uint64_t)1 << 54,
        ((uint64_t)1 << 62) - ((uint64_t)1 << 9),
    };
    static const uint32_t terms[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 0xFFFE0000u, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(sums) / sizeof(sums[0]); i++) {
        for (size_t j = 0; j < sizeof(terms) / sizeof(terms[0]); j++) {
            mu_assert("add_term differs from the double add on an edge pair",
                      add_term_is_double_add(sums[i], terms[j]));
        }
    }
    for (uint32_t k = 0; k < RANDOM_PAIRS; k++) {
        const uint64_t raw =
            ((uint64_t)float_moment_twin_hash(k, 1u, 5u) << 32) | float_moment_twin_hash(k, 2u, 5u);
        const uint64_t sum = (uint64_t)(double)(raw >> (2u + (k % 12u)));
        const uint32_t term =
            float_moment_twin_float_square(float_moment_twin_hash(k, 3u, 5u) & 0xFFFFu);
        mu_assert("add_term differs from the double add", add_term_is_double_add(sum, term));
    }
    return NULL;
}

/* Only a frame whose sum of squares can pass 2^53 units needs the walk. */
static char *test_may_round(void)
{
    mu_assert("2^21 16-bit pixels cannot pass 2^53", !vmaf_moment_sum_may_round(2048u, 1024u, 16u));
    mu_assert("more than 2^21 16-bit pixels can", vmaf_moment_sum_may_round(2048u, 1025u, 16u));
    mu_assert("2^29 12-bit pixels cannot", !vmaf_moment_sum_may_round(32768u, 16384u, 12u));
    mu_assert("more than 2^29 12-bit pixels can", vmaf_moment_sum_may_round(32768u, 16385u, 12u));
    mu_assert("10-bit frames never do", !vmaf_moment_sum_may_round(32768u, 32768u, 10u));
    mu_assert("8-bit frames never do", !vmaf_moment_sum_may_round(32768u, 32768u, 8u));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_add_term_is_the_double_add);
    mu_run_test(test_may_round);
    mu_run_test(test_model_equals_cpu_past_2_53);
    mu_run_test(test_model_equals_cpu_large);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_FLOAT_MOMENT_SUM_MODEL_H_ */
