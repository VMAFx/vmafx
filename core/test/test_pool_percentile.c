/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/* ADR-1188 — percentile temporal pooling (MEDIAN / PERC5 / PERC10 / PERC20).
 *
 * Closes T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03: before the
 * fix these enumerators did not exist and any discriminant past
 * HARMONIC_MEAN was rejected by pool_reduce()'s `default: return -EINVAL`.
 *
 * The reference numbers are `numpy.percentile(..., method="linear")` — the
 * exact rule the Python harness applies via `ListStats.perc10` — so a
 * divergence here means the C API and the harness would report different
 * pooled scores for the same frames. Scores are imported directly through
 * vmaf_import_feature_score, so the test needs no YUV fixture and no model. */

#include <errno.h>
#include <math.h>

#include "test.h"
#include "libvmaf/libvmaf.h"

/* The 48 per-frame VMAF scores of the Netflix golden pair
 * (src01_hrc00_576x324.yuv vs src01_hrc01_576x324.yuv, vmaf_v0.6.1),
 * captured with `vmaf ... --json --precision=max` from this build. */
static const double golden_src01_vmaf[] = {
    83.856285957006605, 82.639802580683209, 81.038578985522264, 81.925763209338399,
    77.463057391559303, 76.445022596993624, 78.696381547909496, 74.045764240998281,
    75.157339285036684, 75.999432253222338, 73.428076463659238, 72.268215272248767,
    77.469371902775535, 73.634989791509426, 72.340314616635766, 74.858285737478269,
    71.174772248606118, 72.711413148949134, 76.442892577786438, 72.719880300510695,
    73.331077203509039, 78.387738884569004, 76.779101236767673, 79.026916583515955,
    87.180960097584446, 80.307239223446388, 78.908532777906572, 79.295299275674338,
    75.199473071309257, 73.96191641989563,  77.580579853598437, 73.867052730799998,
    73.006711382307415, 75.251921567337405, 72.373281664491785, 73.259181875277676,
    78.237247484884662, 73.39705454309744,  73.283495363715389, 76.183895755257794,
    74.745047299751747, 74.552672029553491, 79.636268310023738, 75.897655047136183,
    77.114523352574963, 81.470512788263989, 80.481665296626247, 83.023220196728161,
};

#define GOLDEN_CNT ((unsigned)(sizeof(golden_src01_vmaf) / sizeof(golden_src01_vmaf[0])))

/* numpy.percentile(golden_src01_vmaf, q) for q = 5, 10, 20, 50. */
static const double golden_perc5 = 72.351853083385384;
static const double golden_perc10 = 72.717340155042217;
static const double golden_perc20 = 73.357468139344396;
static const double golden_median = 76.091664004240072;

/* python/test/quality_runner_test.py:679 pins the Python harness's perc10 of
 * the same clip pair at this value with places=2 (tolerance 5e-3). The C
 * engine's per-frame scores differ from the harness's in the last few
 * decimals, so the pooled percentile is compared at the same tolerance the
 * golden assertion itself uses. */
static const double harness_perc10 = 72.71845922683059;
static const double harness_places2 = 5e-3;

static int import_scores(VmafContext *vmaf, const char *name, const double *score, unsigned cnt)
{
    int err = 0;
    for (unsigned i = 0; i < cnt; i++)
        err |= vmaf_import_feature_score(vmaf, name, score[i], i);
    return err;
}

/* Element count of a PoolCase table literal. */
#define POOL_CASE_CNT(t) ((unsigned)(sizeof(t) / sizeof((t)[0])))

/* Propagate a helper's failure message. Mirrors mu_assert for helpers that
 * already return "nullptr on success, or the message to fail with". */
#define mu_assert_msg(expr)                                                                        \
    do {                                                                                           \
        char *mu_helper_msg = (expr);                                                              \
        if (mu_helper_msg)                                                                         \
            return mu_helper_msg;                                                                  \
    } while (0)

/* One pooled-score expectation: pool `name` over [0, last] with `method` and
 * compare against `expect` within `tol`. Returns nullptr on success or `msg`,
 * so callers can drive a table of cases through a single mu_assert instead of
 * one per case (readability-function-size caps a test body at 15 branches and
 * every mu_assert expands to two). */
typedef struct PoolCase {
    enum VmafPoolingMethod method;
    double expect;
    double tol;
    char *msg;
} PoolCase;

static char *check_pool_cases(VmafContext *vmaf, const char *name, unsigned last,
                              const PoolCase *cases, unsigned cnt)
{
    for (unsigned i = 0; i < cnt; i++) {
        double score = 0.;
        if (vmaf_feature_score_pooled(vmaf, name, cases[i].method, &score, 0, last))
            return cases[i].msg;
        if (fabs(score - cases[i].expect) > cases[i].tol)
            return cases[i].msg;
    }
    return nullptr;
}

/* Open a context with `n_subsample` and import `cnt` per-frame scores under
 * `name`. Returns nullptr on success or a static failure message. */
static char *open_with_scores(VmafContext **vmaf, const char *name, const double *score,
                              unsigned cnt, unsigned n_subsample)
{
    VmafConfiguration cfg = {0};
    cfg.n_subsample = n_subsample;

    if (vmaf_init(vmaf, cfg))
        return "problem during vmaf_init";
    if (import_scores(*vmaf, name, score, cnt))
        return "problem during vmaf_import_feature_score";
    return nullptr;
}

/* Percentile pooling over the golden pair's real per-frame scores reproduces
 * numpy.percentile exactly, and lands on the Python harness's golden perc10
 * within the tolerance that assertion uses. */
static char *test_percentile_matches_python_harness(void)
{
    VmafContext *vmaf = nullptr;
    mu_assert_msg(open_with_scores(&vmaf, "vmaf", golden_src01_vmaf, GOLDEN_CNT, 0u));

    static const PoolCase cases[] = {
        {VMAF_POOL_METHOD_PERC10, golden_perc10, 1e-12,
         "PERC10 does not match numpy.percentile(q=10)"},
        {VMAF_POOL_METHOD_PERC10, harness_perc10, harness_places2,
         "PERC10 does not match the Python harness golden perc10"},
        {VMAF_POOL_METHOD_PERC5, golden_perc5, 1e-12, "PERC5 does not match numpy.percentile(q=5)"},
        {VMAF_POOL_METHOD_PERC20, golden_perc20, 1e-12,
         "PERC20 does not match numpy.percentile(q=20)"},
        {VMAF_POOL_METHOD_MEDIAN, golden_median, 1e-12,
         "MEDIAN does not match numpy.percentile(q=50)"},
    };
    mu_assert_msg(check_pool_cases(vmaf, "vmaf", GOLDEN_CNT - 1, cases, POOL_CASE_CNT(cases)));

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return nullptr;
}

/* The interpolation rule itself, on a vector whose percentiles are trivially
 * hand-checkable: numpy.percentile([1,2,3,4], [5,10,20,50]) is
 * [1.15, 1.3, 1.6, 2.5]. An implementation that picked the nearest rank
 * instead of interpolating would return 1.0 / 1.0 / 2.0 / 2.0 here. */
static char *test_percentile_interpolates_between_ranks(void)
{
    VmafContext *vmaf = nullptr;
    static const double v[] = {1., 2., 3., 4.};
    mu_assert_msg(open_with_scores(&vmaf, "f", v, 4u, 0u));

    static const PoolCase cases[] = {
        {VMAF_POOL_METHOD_PERC5, 1.15, 1e-12, "PERC5 is not the interpolated 1.15"},
        {VMAF_POOL_METHOD_PERC10, 1.3, 1e-12, "PERC10 is not the interpolated 1.3"},
        {VMAF_POOL_METHOD_PERC20, 1.6, 1e-12, "PERC20 is not the interpolated 1.6"},
        {VMAF_POOL_METHOD_MEDIAN, 2.5, 1e-12, "MEDIAN is not the interpolated 2.5"},
    };
    mu_assert_msg(check_pool_cases(vmaf, "f", 3, cases, POOL_CASE_CNT(cases)));

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return nullptr;
}

/* Percentiles are order statistics over the frames that pooling actually
 * visits: unsorted input must not matter, and a single frame must pool to
 * itself for every rank. */
static char *test_percentile_order_and_single_frame(void)
{
    VmafContext *vmaf = nullptr;
    static const double shuffled[] = {4., 1., 3., 2.};
    mu_assert_msg(open_with_scores(&vmaf, "f", shuffled, 4u, 0u));

    static const PoolCase cases[] = {
        {VMAF_POOL_METHOD_MEDIAN, 2.5, 1e-12, "MEDIAN depends on arrival order"},
    };
    mu_assert_msg(check_pool_cases(vmaf, "f", 3, cases, POOL_CASE_CNT(cases)));

    double score = 0.;
    mu_assert("single-frame PERC5 pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_PERC5, &score, 2, 2));
    mu_assert("single-frame PERC5 is not that frame's score", fabs(score - 3.) < 1e-12);

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return nullptr;
}

/* n_subsample must skip the same frames for percentiles as for the
 * accumulator methods: with n_subsample = 2 only the even indices are pooled,
 * so the median of [1..4] becomes the median of {1, 3} = 2.0, not 2.5. */
static char *test_percentile_honours_n_subsample(void)
{
    VmafContext *vmaf = nullptr;
    static const double v[] = {1., 2., 3., 4.};
    mu_assert_msg(open_with_scores(&vmaf, "f", v, 4u, 2u));

    static const PoolCase cases[] = {
        {VMAF_POOL_METHOD_MEDIAN, 2., 1e-12, "MEDIAN ignored n_subsample"},
        {VMAF_POOL_METHOD_MEAN, 2., 1e-12,
         "MEAN and MEDIAN disagree about which frames were pooled"},
    };
    mu_assert_msg(check_pool_cases(vmaf, "f", 3, cases, POOL_CASE_CNT(cases)));

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return nullptr;
}

/* An out-of-range discriminant a future ABI might carry. Reproducing exactly
 * this call — a caller built against a newer header handing an older library a
 * pooling method it has never heard of — is the whole point of
 * test_invalid_pool_methods_still_rejected (ADR-1188: the enum grows
 * append-only, so the guard that rejects unknown discriminants is what keeps
 * that growth safe). The cast therefore cannot be refactored away, and
 * clang-analyzer flags it by construction. */
static enum VmafPoolingMethod future_abi_pool_method(void)
{
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) — ADR-1188 */
    return (enum VmafPoolingMethod)99;
}
/* The append-only enum growth must not weaken the discriminant guards: the
 * UNKNOWN sentinel and any out-of-range value are still rejected, and the
 * legacy accumulator methods keep their values and results. (The
 * VMAF_POOL_METHOD_NB count sentinel is deliberately not referenced here — it
 * is deprecated in the public header; core/src/output.cpp static_asserts its
 * value instead.) */
static char *test_invalid_pool_methods_still_rejected(void)
{
    VmafContext *vmaf = nullptr;
    static const double v[] = {1., 2., 3., 4.};
    mu_assert_msg(open_with_scores(&vmaf, "f", v, 4u, 0u));

    double score = 0.;
    mu_assert("UNKNOWN pool method was accepted",
              vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_UNKNOWN, &score, 0, 3) ==
                  -EINVAL);
    mu_assert("out-of-range pool method was accepted",
              vmaf_feature_score_pooled(vmaf, "f", future_abi_pool_method(), &score, 0, 3) ==
                  -EINVAL);
    mu_assert("null score pointer was accepted",
              vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEDIAN, nullptr, 0, 3) ==
                  -EINVAL);

    static const PoolCase cases[] = {
        {VMAF_POOL_METHOD_MIN, 1., 1e-12, "MIN changed"},
        {VMAF_POOL_METHOD_MAX, 4., 1e-12, "MAX changed"},
        {VMAF_POOL_METHOD_MEAN, 2.5, 1e-12, "MEAN changed"},
    };
    mu_assert_msg(check_pool_cases(vmaf, "f", 3, cases, POOL_CASE_CNT(cases)));

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return nullptr;
}

/* Enumerator values are append-only: every pre-existing discriminant keeps the
 * integer an already-compiled consumer baked in. Driven off a table so the
 * body stays inside the branch budget. */
static char *test_enum_values_are_append_only(void)
{
    static const struct {
        enum VmafPoolingMethod method;
        int value;
        char *msg;
    } expected[] = {
        {VMAF_POOL_METHOD_UNKNOWN, 0, "VMAF_POOL_METHOD_UNKNOWN moved"},
        {VMAF_POOL_METHOD_MIN, 1, "VMAF_POOL_METHOD_MIN moved"},
        {VMAF_POOL_METHOD_MAX, 2, "VMAF_POOL_METHOD_MAX moved"},
        {VMAF_POOL_METHOD_MEAN, 3, "VMAF_POOL_METHOD_MEAN moved"},
        {VMAF_POOL_METHOD_HARMONIC_MEAN, 4, "VMAF_POOL_METHOD_HARMONIC_MEAN moved"},
        {VMAF_POOL_METHOD_MEDIAN, 5, "VMAF_POOL_METHOD_MEDIAN is not appended after HARMONIC_MEAN"},
        {VMAF_POOL_METHOD_PERC5, 6, "VMAF_POOL_METHOD_PERC5 is not appended after MEDIAN"},
        {VMAF_POOL_METHOD_PERC10, 7, "VMAF_POOL_METHOD_PERC10 is not appended after PERC5"},
        {VMAF_POOL_METHOD_PERC20, 8, "VMAF_POOL_METHOD_PERC20 is not appended after PERC10"},
    };

    char *failure = nullptr;
    for (unsigned i = 0; i < (unsigned)(sizeof(expected) / sizeof(expected[0])); i++) {
        if ((int)expected[i].method != expected[i].value)
            failure = expected[i].msg;
    }
    mu_assert_msg(failure);
    return nullptr;
}

mu_message_t run_tests(void)
{
    mu_run_test(test_percentile_matches_python_harness);
    mu_run_test(test_percentile_interpolates_between_ranks);
    mu_run_test(test_percentile_order_and_single_frame);
    mu_run_test(test_percentile_honours_n_subsample);
    mu_run_test(test_invalid_pool_methods_still_rejected);
    mu_run_test(test_enum_values_are_append_only);
    return nullptr;
}
