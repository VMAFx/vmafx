/**
 * Copyright 2026 Lusoris
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* The flush drain loop must terminate (docs/state.md
 * T-CUDA-SINGLE-FRAME-HANG-2026-09-05).
 *
 * `vmaf_feature_extractor_context_flush` drains an extractor with
 * `while (!(err = fex->flush(fex, vfc)))` — a flush that keeps returning 0 spins
 * forever. The CUDA motion twin's single-frame path back-fills motion3 for index
 * 0 and used to return the append result, which is 0, so `vmaf --backend cuda
 * --frame_cnt 1` never exited. The contract is: append at most once, then report
 * 1 ("nothing more to append"), exactly like the legacy path and the CPU twin.
 *
 * This test pins the contract without a GPU: it drives a stub extractor whose
 * flush mimics the fixed shape and asserts the drain loop ends. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "test.h"

/* Mirrors the fixed single-frame flush: append once, then say "no more". */
static unsigned stub_flush_calls;

static int stub_flush_single_frame(VmafFeatureExtractor *fex, VmafFeatureCollector *vfc)
{
    (void)fex;
    stub_flush_calls++;
    double existing = 0.;
    if (vmaf_feature_collector_get_score(vfc, "stub_motion3", &existing, 0) == 0)
        return 1; /* already written — nothing more to append */
    const int err = vmaf_feature_collector_append(vfc, "stub_motion3", 0., 0);
    return (err < 0) ? err : 1;
}

/* The pre-fix shape, kept as the negative control: it returns the append result
 * (0) on the first call and 0 again on every later call, so a drain loop over it
 * never terminates. The test calls it a bounded number of times instead of
 * looping, and asserts it keeps claiming "appended". */
static int stub_flush_never_terminates(VmafFeatureExtractor *fex, VmafFeatureCollector *vfc)
{
    (void)fex;
    double existing = 0.;
    if (vmaf_feature_collector_get_score(vfc, "stub_bad", &existing, 0) == 0)
        return 0; /* the bug: "appended" forever */
    return vmaf_feature_collector_append(vfc, "stub_bad", 0., 0);
}

static char *test_single_frame_flush_terminates(void)
{
    VmafFeatureCollector *vfc = NULL;
    int err = vmaf_feature_collector_init(&vfc);
    mu_assert("feature collector init", err == 0);

    stub_flush_calls = 0;
    unsigned guard = 0;
    while (!stub_flush_single_frame(NULL, vfc)) {
        if (++guard > 16U)
            break;
    }
    mu_assert("the drain loop must end", guard <= 16U);
    mu_assert("flush is called at most twice (append, then 'no more')", stub_flush_calls <= 2U);

    double score = -1.;
    err = vmaf_feature_collector_get_score(vfc, "stub_motion3", &score, 0);
    mu_assert("the single-frame back-fill happened exactly once", err == 0 && score == 0.);

    vmaf_feature_collector_destroy(vfc);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

static char *test_pre_fix_shape_would_spin(void)
{
    VmafFeatureCollector *vfc = NULL;
    int err = vmaf_feature_collector_init(&vfc);
    mu_assert("feature collector init", err == 0);

    /* First call appends and returns 0; the second still returns 0. A real drain
     * loop would never leave. This is the negative control for the contract. */
    mu_assert("pre-fix flush claims 'appended' on the first call",
              stub_flush_never_terminates(NULL, vfc) == 0);
    mu_assert("pre-fix flush still claims 'appended' on the second call",
              stub_flush_never_terminates(NULL, vfc) == 0);

    vmaf_feature_collector_destroy(vfc);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_single_frame_flush_terminates);
    mu_run_test(test_pre_fix_shape_would_spin);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

int tests_run = 0;
