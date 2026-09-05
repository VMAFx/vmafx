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

/* Percentile pooling over the golden pair's real per-frame scores reproduces
 * numpy.percentile exactly, and lands on the Python harness's golden perc10
 * within the tolerance that assertion uses. */
static char *test_percentile_matches_python_harness(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);
    mu_assert("problem during vmaf_import_feature_score",
              !import_scores(vmaf, "vmaf", golden_src01_vmaf, GOLDEN_CNT));

    double score = 0.;
    err =
        vmaf_feature_score_pooled(vmaf, "vmaf", VMAF_POOL_METHOD_PERC10, &score, 0, GOLDEN_CNT - 1);
    mu_assert("PERC10 pooling failed", !err);
    mu_assert("PERC10 does not match numpy.percentile(q=10)", fabs(score - golden_perc10) < 1e-12);
    mu_assert("PERC10 does not match the Python harness golden perc10",
              fabs(score - harness_perc10) < harness_places2);

    err =
        vmaf_feature_score_pooled(vmaf, "vmaf", VMAF_POOL_METHOD_PERC5, &score, 0, GOLDEN_CNT - 1);
    mu_assert("PERC5 pooling failed", !err);
    mu_assert("PERC5 does not match numpy.percentile(q=5)", fabs(score - golden_perc5) < 1e-12);

    err =
        vmaf_feature_score_pooled(vmaf, "vmaf", VMAF_POOL_METHOD_PERC20, &score, 0, GOLDEN_CNT - 1);
    mu_assert("PERC20 pooling failed", !err);
    mu_assert("PERC20 does not match numpy.percentile(q=20)", fabs(score - golden_perc20) < 1e-12);

    err =
        vmaf_feature_score_pooled(vmaf, "vmaf", VMAF_POOL_METHOD_MEDIAN, &score, 0, GOLDEN_CNT - 1);
    mu_assert("MEDIAN pooling failed", !err);
    mu_assert("MEDIAN does not match numpy.percentile(q=50)", fabs(score - golden_median) < 1e-12);

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

/* The interpolation rule itself, on a vector whose percentiles are trivially
 * hand-checkable: numpy.percentile([1,2,3,4], [5,10,20,50]) is
 * [1.15, 1.3, 1.6, 2.5]. An implementation that picked the nearest rank
 * instead of interpolating would return 1.0 / 1.0 / 2.0 / 2.0 here. */
static char *test_percentile_interpolates_between_ranks(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};
    static const double v[] = {1., 2., 3., 4.};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);
    mu_assert("problem during vmaf_import_feature_score", !import_scores(vmaf, "f", v, 4u));

    double score = 0.;
    mu_assert("PERC5 pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_PERC5, &score, 0, 3));
    mu_assert("PERC5 is not the interpolated 1.15", fabs(score - 1.15) < 1e-12);
    mu_assert("PERC10 pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_PERC10, &score, 0, 3));
    mu_assert("PERC10 is not the interpolated 1.3", fabs(score - 1.3) < 1e-12);
    mu_assert("PERC20 pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_PERC20, &score, 0, 3));
    mu_assert("PERC20 is not the interpolated 1.6", fabs(score - 1.6) < 1e-12);
    mu_assert("MEDIAN pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEDIAN, &score, 0, 3));
    mu_assert("MEDIAN is not the interpolated 2.5", fabs(score - 2.5) < 1e-12);

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

/* Percentiles are order statistics over the frames that pooling actually
 * visits: unsorted input must not matter, and a single frame must pool to
 * itself for every rank. */
static char *test_percentile_order_and_single_frame(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};
    static const double shuffled[] = {4., 1., 3., 2.};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);
    mu_assert("problem during vmaf_import_feature_score", !import_scores(vmaf, "f", shuffled, 4u));

    double score = 0.;
    mu_assert("MEDIAN pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEDIAN, &score, 0, 3));
    mu_assert("MEDIAN depends on arrival order", fabs(score - 2.5) < 1e-12);

    mu_assert("single-frame PERC5 pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_PERC5, &score, 2, 2));
    mu_assert("single-frame PERC5 is not that frame's score", fabs(score - 3.) < 1e-12);

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

/* n_subsample must skip the same frames for percentiles as for the
 * accumulator methods: with n_subsample = 2 only the even indices are pooled,
 * so the median of [1..4] becomes the median of {1, 3} = 2.0, not 2.5. */
static char *test_percentile_honours_n_subsample(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};
    static const double v[] = {1., 2., 3., 4.};
    cfg.n_subsample = 2;

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);
    mu_assert("problem during vmaf_import_feature_score", !import_scores(vmaf, "f", v, 4u));

    double score = 0.;
    mu_assert("MEDIAN pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEDIAN, &score, 0, 3));
    mu_assert("MEDIAN ignored n_subsample", fabs(score - 2.) < 1e-12);

    double mean = 0.;
    mu_assert("MEAN pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEAN, &mean, 0, 3));
    mu_assert("MEAN and MEDIAN disagree about which frames were pooled", fabs(mean - 2.) < 1e-12);

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

/* The append-only enum growth must not weaken the discriminant guards: the
 * UNKNOWN sentinel and any out-of-range value are still rejected, and the
 * legacy accumulator methods keep their values and results. */
static char *test_invalid_pool_methods_still_rejected(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};
    static const double v[] = {1., 2., 3., 4.};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);
    mu_assert("problem during vmaf_import_feature_score", !import_scores(vmaf, "f", v, 4u));

    double score = 0.;
    mu_assert("UNKNOWN pool method was accepted",
              vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_UNKNOWN, &score, 0, 3) ==
                  -EINVAL);
    /* An out-of-range discriminant a future ABI might carry. The cast is the
     * point of the test: it reproduces what a caller built against a newer
     * header would pass to an older library. */
    mu_assert("out-of-range pool method was accepted",
              vmaf_feature_score_pooled(vmaf, "f", (enum VmafPoolingMethod)99, &score, 0, 3) ==
                  -EINVAL);
    mu_assert("NULL score pointer was accepted",
              vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEDIAN, NULL, 0, 3) == -EINVAL);

    mu_assert("MIN pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MIN, &score, 0, 3));
    mu_assert("MIN changed", fabs(score - 1.) < 1e-12);
    mu_assert("MAX pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MAX, &score, 0, 3));
    mu_assert("MAX changed", fabs(score - 4.) < 1e-12);
    mu_assert("MEAN pooling failed",
              !vmaf_feature_score_pooled(vmaf, "f", VMAF_POOL_METHOD_MEAN, &score, 0, 3));
    mu_assert("MEAN changed", fabs(score - 2.5) < 1e-12);

    mu_assert("problem during vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

/* Enumerator values are append-only: every pre-existing discriminant keeps the
 * integer an already-compiled consumer baked in. */
static char *test_enum_values_are_append_only(void)
{
    mu_assert("VMAF_POOL_METHOD_UNKNOWN moved", VMAF_POOL_METHOD_UNKNOWN == 0);
    mu_assert("VMAF_POOL_METHOD_MIN moved", VMAF_POOL_METHOD_MIN == 1);
    mu_assert("VMAF_POOL_METHOD_MAX moved", VMAF_POOL_METHOD_MAX == 2);
    mu_assert("VMAF_POOL_METHOD_MEAN moved", VMAF_POOL_METHOD_MEAN == 3);
    mu_assert("VMAF_POOL_METHOD_HARMONIC_MEAN moved", VMAF_POOL_METHOD_HARMONIC_MEAN == 4);
    mu_assert("VMAF_POOL_METHOD_MEDIAN is not appended after HARMONIC_MEAN",
              VMAF_POOL_METHOD_MEDIAN == 5);
    mu_assert("VMAF_POOL_METHOD_PERC5 is not appended after MEDIAN", VMAF_POOL_METHOD_PERC5 == 6);
    mu_assert("VMAF_POOL_METHOD_PERC10 is not appended after PERC5", VMAF_POOL_METHOD_PERC10 == 7);
    mu_assert("VMAF_POOL_METHOD_PERC20 is not appended after PERC10", VMAF_POOL_METHOD_PERC20 == 8);
    return NULL;
}

mu_message_t run_tests(void)
{
    mu_run_test(test_percentile_matches_python_harness);
    mu_run_test(test_percentile_interpolates_between_ranks);
    mu_run_test(test_percentile_order_and_single_frame);
    mu_run_test(test_percentile_honours_n_subsample);
    mu_run_test(test_invalid_pool_methods_still_rejected);
    mu_run_test(test_enum_values_are_append_only);
    return NULL;
}
