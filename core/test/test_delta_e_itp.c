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

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
// NOLINTNEXTLINE(bugprone-suspicious-include): the test deliberately includes the .c to exercise the static per-pixel ITP helpers (rgb_pq_to_itp / delta_e_itp_pair) against the BT.2124-0 oracle — the same pattern as test_ciede.c.
#include "feature/delta_e_itp.c"

#include "dict.h"
#include "feature/feature_collector.h"
#include "libvmaf/picture.h"

#define DEITP_W (16u)
#define DEITP_H (16u)

/* Fill a single 8-bit plane with a constant value. */
static void fill_plane8(VmafPicture *pic, unsigned plane, uint8_t v)
{
    uint8_t *p = (uint8_t *)pic->data[plane];
    const ptrdiff_t s = pic->stride[plane];
    for (unsigned r = 0; r < pic->h[plane]; ++r) {
        memset(p + r * s, v, pic->w[plane]);
    }
}

/* Allocate an 8-bit YUV420P picture with constant Y/U/V. */
static int alloc_const_420(VmafPicture *pic, uint8_t y, uint8_t u, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, DEITP_W, DEITP_H);
    if (err) {
        return err;
    }
    fill_plane8(pic, 0, y);
    fill_plane8(pic, 1, u);
    fill_plane8(pic, 2, v);
    return 0;
}

/* places=4 tolerance (1e-4), matching the BT.2124-0 ITP-triple parity
 * bound recommended by the metric dossier and the fork's "GPUs not
 * bit-exact" places=4 convention (ADR-0214). */
static int close_4dp(double a, double b)
{
    return fabs(a - b) < 1e-4;
}

/* ITU-R BT.2124-0 Annex 4 normative worked example: the BT.2111 58% PQ
 * BT.709-blue patch, 10-bit FULL-range PQ RGB code values [296, 201, 582].
 *
 * The oracle is the FULL-PRECISION ITP triple [0.355721, 0.134647,
 * -0.161395] (rounds to the standard's printed [0.3557, 0.1346,
 * -0.1614]).  Asserting the full-precision triple is the robust parity
 * gate; the standard's pooled ΔE = 2.363 uses pre-rounded intermediates
 * and is NOT used as the extractor oracle (it is only a documentation
 * cross-check; see test_delta_e_itp_doc_rounded_pair below). */
static char *test_delta_e_itp_blue_patch_oracle(void)
{
    const double ep[3] = {296.0 / 1023.0, 201.0 / 1023.0, 582.0 / 1023.0};
    double itp[3];
    rgb_pq_to_itp(ep[0], ep[1], ep[2], itp);

    mu_assert("ITP I should be 0.355721 (BT.2124-0 Annex 4)", close_4dp(itp[0], 0.355721));
    mu_assert("ITP T should be 0.134647 (BT.2124-0 Annex 4)", close_4dp(itp[1], 0.134647));
    mu_assert("ITP P should be -0.161395 (BT.2124-0 Annex 4)", close_4dp(itp[2], -0.161395));

    return NULL;
}

/* Identity: an unchanged pixel must yield exactly 0 ΔE-ITP. */
static char *test_delta_e_itp_identity(void)
{
    const double ep[3] = {296.0 / 1023.0, 201.0 / 1023.0, 582.0 / 1023.0};
    double itp[3];
    rgb_pq_to_itp(ep[0], ep[1], ep[2], itp);

    const double de = delta_e_itp_pair(itp, itp);
    mu_assert("identity ΔE-ITP must be exactly 0", de == 0.0);

    return NULL;
}

/* Full-precision ΔE-ITP for a fully-specified pair of linear-RGB
 * triples, host-independent and unambiguous.  Reference value computed
 * by the verified Python pipeline that reproduces the BT.2124-0 oracle:
 *   ref  linear RGB = PQ_EOTF([296,201,582]/1023)  (blue patch)
 *   dist linear RGB = [10.0, 3.0, 175.0]           (nearby HDR colour)
 *   ΔE-ITP = 8.037360. */
static char *test_delta_e_itp_synthetic_pair(void)
{
    const double ref_ep[3] = {296.0 / 1023.0, 201.0 / 1023.0, 582.0 / 1023.0};
    double ref_itp[3];
    rgb_pq_to_itp(ref_ep[0], ref_ep[1], ref_ep[2], ref_itp);

    /* dist is specified directly in linear RGB (cd/m^2); feed it through
     * the PQ inverse EOTF so rgb_pq_to_itp's leading PQ EOTF round-trips
     * back to the intended linear values. */
    double dist_ep[3];
    dist_ep[0] = vmaf_pq_eotf_inv(10.0);
    dist_ep[1] = vmaf_pq_eotf_inv(3.0);
    dist_ep[2] = vmaf_pq_eotf_inv(175.0);
    double dist_itp[3];
    rgb_pq_to_itp(dist_ep[0], dist_ep[1], dist_ep[2], dist_itp);

    const double de = delta_e_itp_pair(ref_itp, dist_itp);
    mu_assert("ΔE-ITP for the synthetic pair should be 8.037360", close_4dp(de, 8.037360));

    return NULL;
}

/* Documentation cross-check: the standard's pooled ΔE = 2.363 is
 * reproduced from its PRE-ROUNDED 4-dp ITP triples (NOT the extractor's
 * full-precision output).  Asserted at places=3 since the standard only
 * prints 3 dp. */
static char *test_delta_e_itp_doc_rounded_pair(void)
{
    const double itp1[3] = {0.3554, 0.1346, -0.1613};
    const double itp2[3] = {0.3568, 0.1321, -0.1629};
    const double de = delta_e_itp_pair(itp1, itp2);
    mu_assert("ΔE-ITP of the BT.2124-0 rounded triples should be 2.363 (places=3)",
              fabs(de - 2.363) < 1e-3);
    return NULL;
}

/* PQ transfer functions round-trip and hit the documented anchors:
 * the blue-patch E' decodes to ~181.318 cd/m^2 on the blue channel
 * (BT.2124-0 Annex 4), and EOTF(EOTF^-1(x)) == x. */
static char *test_pq_transfer_roundtrip(void)
{
    const double linear = vmaf_pq_eotf(582.0 / 1023.0);
    mu_assert("PQ EOTF of the blue-patch B' should be ~181.318 cd/m^2",
              fabs(linear - 181.318) < 1e-2);

    const double rt = vmaf_pq_eotf(vmaf_pq_eotf_inv(181.318));
    mu_assert("PQ EOTF(EOTF^-1(L)) must round-trip", fabs(rt - 181.318) < 1e-6);

    mu_assert("PQ EOTF(0) == 0", vmaf_pq_eotf(0.0) == 0.0);

    return NULL;
}

/* Run the registered delta_e_itp extractor (default options) on one
 * 8-bit YUV420P (ref, dist) frame pair and return the pooled score.
 * Returns -1 on any pipeline error. */
static double run_extractor_pair(uint8_t ry, uint8_t ru, uint8_t rv, uint8_t dy, uint8_t du,
                                 uint8_t dv)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("delta_e_itp");
    if (!fex) {
        return -1.0;
    }

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    VmafPicture ref;
    VmafPicture dist;
    memset(&ref, 0, sizeof(ref));
    memset(&dist, 0, sizeof(dist));
    double score = -1.0;

    if (vmaf_feature_extractor_context_create(&ctx, fex, NULL)) {
        goto cleanup;
    }
    if (vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, DEITP_W, DEITP_H)) {
        goto cleanup;
    }
    if (vmaf_feature_collector_init(&fc)) {
        goto cleanup;
    }
    if (alloc_const_420(&ref, ry, ru, rv)) {
        goto cleanup;
    }
    if (alloc_const_420(&dist, dy, du, dv)) {
        goto cleanup;
    }

    if (!vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc)) {
        if (vmaf_feature_collector_get_score(fc, "delta_e_itp", &score, 0)) {
            score = -1.0;
        }
    }

cleanup:
    if (ctx) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
    if (fc) {
        vmaf_feature_collector_destroy(fc);
    }
    if (ref.ref != NULL) {
        vmaf_picture_unref(&ref);
    }
    if (dist.ref != NULL) {
        vmaf_picture_unref(&dist);
    }
    return score;
}

/* End-to-end: the registered extractor must load and score an identical
 * (ref == dist) frame as exactly 0, and a distorted frame as a finite
 * strictly-positive value.  Exercises the full init/extract/close path
 * with default options (transfer=pq, matrix=bt2020, range=limited). */
static char *test_delta_e_itp_extractor_end_to_end(void)
{
    const double identity = run_extractor_pair(200u, 128u, 128u, 200u, 128u, 128u);
    mu_assert("identity delta_e_itp must be exactly 0", identity == 0.0);

    const double distorted = run_extractor_pair(200u, 128u, 128u, 120u, 150u, 110u);
    mu_assert("distorted delta_e_itp is finite", isfinite(distorted));
    mu_assert("distorted delta_e_itp is strictly positive", distorted > 0.0);

    return NULL;
}

/* Unsupported transfer (HLG) must be rejected by init with -EINVAL
 * (RC ships PQ only). */
static char *test_delta_e_itp_rejects_non_pq_transfer(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("delta_e_itp");
    mu_assert("delta_e_itp extractor missing from registry", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "transfer", "hlg", 0);
    mu_assert("set transfer=hlg", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with opts", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, DEITP_W, DEITP_H);
    mu_assert("init must reject transfer=hlg with -EINVAL", err == -EINVAL);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_delta_e_itp_blue_patch_oracle);
    mu_run_test(test_delta_e_itp_identity);
    mu_run_test(test_delta_e_itp_synthetic_pair);
    mu_run_test(test_delta_e_itp_doc_rounded_pair);
    mu_run_test(test_pq_transfer_roundtrip);
    mu_run_test(test_delta_e_itp_extractor_end_to_end);
    mu_run_test(test_delta_e_itp_rejects_non_pq_transfer);
    return NULL;
}
