/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include "test.h"
// NOLINTNEXTLINE(bugprone-suspicious-include): white-box test deliberately includes ciede.c to reach static scale_chroma_planes helpers (ADR-0141 / ADR-0278).
#include "feature/ciede.c"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

static int close_enough(float a, float b)
{
    const float epsilon = 1e-9f;
    return fabsf(a - b) < epsilon;
}

static void seed_chroma_422_8b(VmafPicture *in)
{
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *buf = in->data[p];
        for (unsigned i = 0; i < in->h[p]; i++) {
            for (unsigned j = 0; j < in->w[p]; j++)
                buf[j] = (uint8_t)(1u + p * 50u + i * 8u + j);
            buf += in->stride[p];
        }
    }
}

static char *verify_plane_422_8b(const uint8_t *in_buf, ptrdiff_t in_stride, const uint8_t *out_buf,
                                 ptrdiff_t out_stride, unsigned w, unsigned h)
{
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++) {
            const uint8_t expected = in_buf[i * in_stride + (j / 2)];
            mu_assert("422 8b chroma upsample value mismatch",
                      out_buf[i * out_stride + j] == expected);
        }
    }
    return NULL;
}

static char *verify_chroma_422_8b(const VmafPicture *in, const VmafPicture *out)
{
    for (unsigned p = 1; p < 3; p++) {
        char *msg = verify_plane_422_8b(in->data[p], in->stride[p], out->data[p], out->stride[p],
                                        out->w[p], out->h[p]);
        if (msg)
            return msg;
    }
    return NULL;
}

/* Regression for the CIEDE 4:2:2 chroma-upsample flag swap (heap OOB read +
 * wrong scores). scale_chroma_planes must use ss_hor for the horizontal index
 * and ss_ver for the vertical row advance. For 4:2:2 (ss_hor=1, ss_ver=0)
 * every output column j must read input column j/2 from the SAME row, never
 * past the half-width input row and never skipping rows. We seed each chroma
 * sample uniquely so an incorrect divisor/advance produces a detectable miss.
 */
static char *test_ciede_scale_chroma_422_8b(void)
{
    /* 6x4 luma => 3x4 chroma for 4:2:2 (half width, full height). */
    const unsigned w = 6;
    const unsigned h = 4;

    VmafPicture in;
    VmafPicture out;
    int err = vmaf_picture_alloc(&in, VMAF_PIX_FMT_YUV422P, 8, w, h);
    mu_assert("422 input alloc failed", err == 0);
    err = vmaf_picture_alloc(&out, VMAF_PIX_FMT_YUV444P, 8, w, h);
    mu_assert("444 output alloc failed", err == 0);

    /* Plane 0 (luma) is copied 1:1; planes 1/2 carry distinct chroma so the
     * upsample pattern is unambiguous. Seed input chroma row-major with a
     * value that encodes (row, col). */
    seed_chroma_422_8b(&in);

    scale_chroma_planes(&in, &out);

    char *msg = verify_chroma_422_8b(&in, &out);

    (void)vmaf_picture_unref(&in);
    (void)vmaf_picture_unref(&out);
    return msg;
}

static void seed_chroma_422_16b(VmafPicture *in)
{
    for (unsigned p = 1; p < 3; p++) {
        uint16_t *buf = in->data[p];
        const ptrdiff_t stride16 = in->stride[p] / 2;
        for (unsigned i = 0; i < in->h[p]; i++) {
            for (unsigned j = 0; j < in->w[p]; j++)
                buf[j] = (uint16_t)(100u + p * 200u + i * 16u + j);
            buf += stride16;
        }
    }
}

static char *verify_plane_422_16b(const uint16_t *in_buf, ptrdiff_t in_stride16,
                                  const uint16_t *out_buf, ptrdiff_t out_stride16, unsigned w,
                                  unsigned h)
{
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++) {
            const uint16_t expected = in_buf[i * in_stride16 + (j / 2)];
            mu_assert("422 16b chroma upsample value mismatch",
                      out_buf[i * out_stride16 + j] == expected);
        }
    }
    return NULL;
}

static char *verify_chroma_422_16b(const VmafPicture *in, const VmafPicture *out)
{
    for (unsigned p = 1; p < 3; p++) {
        char *msg = verify_plane_422_16b(in->data[p], in->stride[p] / 2, out->data[p],
                                         out->stride[p] / 2, out->w[p], out->h[p]);
        if (msg)
            return msg;
    }
    return NULL;
}

static char *test_ciede_scale_chroma_422_16b(void)
{
    const unsigned w = 6;
    const unsigned h = 4;

    VmafPicture in;
    VmafPicture out;
    int err = vmaf_picture_alloc(&in, VMAF_PIX_FMT_YUV422P, 10, w, h);
    mu_assert("422 hbd input alloc failed", err == 0);
    err = vmaf_picture_alloc(&out, VMAF_PIX_FMT_YUV444P, 10, w, h);
    mu_assert("444 hbd output alloc failed", err == 0);

    seed_chroma_422_16b(&in);

    scale_chroma_planes_hbd(&in, &out);

    char *msg = verify_chroma_422_16b(&in, &out);

    (void)vmaf_picture_unref(&in);
    (void)vmaf_picture_unref(&out);
    return msg;
}

static const KSubArgs default_ksub = {.l = 0.65f, .c = 1.0f, .h = 4.0f};

static char *test_ciede(void)
{
    const LABColor color_1 = {.l = 0.052488625f, .a = -0.587470829f, .b = -8.98771572f};
    const LABColor color_2 = {.l = 0.465437293f, .a = 0.386364758f, .b = -12.7648535f};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 2.54780269", close_enough(de00, 2.54780269f));

    return NULL;
}

static char *test_ciede2(void)
{
    const LABColor color_1 = {.l = 87.156334f, .a = -12.049645f, .b = -1.205325f};
    const LABColor color_2 = {.l = 83.455727f, .a = -9.040445f, .b = -8.894289f};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 4.22714281", close_enough(de00, 4.22714281f));

    return NULL;
}

static char *test_ciede3(void)
{
    const LABColor color_1 = {.l = 79.718491f, .a = 9.109915f, .b = 13.727915f};
    const LABColor color_2 = {.l = 78.717224f, .a = 7.526546f, .b = 5.597448f};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 4.26012468", close_enough(de00, 4.26012468f));

    return NULL;
}

static char *test_ciede4(void)
{
    const LABColor color_1 = {.l = 99.205299f, .a = -3.339410f, .b = 1.205873f};
    const LABColor color_2 = {.l = 97.991730f, .a = -2.497345f, .b = 2.473533f};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 1.26915979", close_enough(de00, 1.26915979f));

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede);
    mu_run_test(test_ciede2);
    mu_run_test(test_ciede3);
    mu_run_test(test_ciede4);
    mu_run_test(test_ciede_scale_chroma_422_8b);
    mu_run_test(test_ciede_scale_chroma_422_16b);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
