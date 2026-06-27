/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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
#include "feature/ciede.c"

static int close_enough(float a, float b)
{
    const float epsilon = 1e-9f;
    return fabs(a - b) < epsilon;
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
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *buf = in.data[p];
        for (unsigned i = 0; i < in.h[p]; i++) {
            for (unsigned j = 0; j < in.w[p]; j++)
                buf[j] = (uint8_t)(1u + p * 50u + i * 8u + j);
            buf += in.stride[p];
        }
    }

    scale_chroma_planes(&in, &out);

    for (unsigned p = 1; p < 3; p++) {
        const uint8_t *in_buf = in.data[p];
        const uint8_t *out_buf = out.data[p];
        for (unsigned i = 0; i < out.h[p]; i++) {
            for (unsigned j = 0; j < out.w[p]; j++) {
                /* Horizontal nearest-neighbour doubling, same row. */
                const uint8_t expected = in_buf[i * in.stride[p] + (j / 2)];
                mu_assert("422 8b chroma upsample value mismatch",
                          out_buf[i * out.stride[p] + j] == expected);
            }
        }
    }

    (void)vmaf_picture_unref(&in);
    (void)vmaf_picture_unref(&out);
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

    for (unsigned p = 1; p < 3; p++) {
        uint16_t *buf = in.data[p];
        const ptrdiff_t stride16 = in.stride[p] / 2;
        for (unsigned i = 0; i < in.h[p]; i++) {
            for (unsigned j = 0; j < in.w[p]; j++)
                buf[j] = (uint16_t)(100u + p * 200u + i * 16u + j);
            buf += stride16;
        }
    }

    scale_chroma_planes_hbd(&in, &out);

    for (unsigned p = 1; p < 3; p++) {
        const uint16_t *in_buf = in.data[p];
        const uint16_t *out_buf = out.data[p];
        const ptrdiff_t in_stride16 = in.stride[p] / 2;
        const ptrdiff_t out_stride16 = out.stride[p] / 2;
        for (unsigned i = 0; i < out.h[p]; i++) {
            for (unsigned j = 0; j < out.w[p]; j++) {
                const uint16_t expected = in_buf[i * in_stride16 + (j / 2)];
                mu_assert("422 16b chroma upsample value mismatch",
                          out_buf[i * out_stride16 + j] == expected);
            }
        }
    }

    (void)vmaf_picture_unref(&in);
    (void)vmaf_picture_unref(&out);
    return NULL;
}

static const KSubArgs default_ksub = {.l = 0.65, .c = 1.0, .h = 4.0};

static char *test_ciede()
{
    const LABColor color_1 = {.l = 0.052488625, .a = -0.587470829, .b = -8.98771572};
    const LABColor color_2 = {.l = 0.465437293, .a = 0.386364758, .b = -12.7648535};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 2.54780269", close_enough(de00, 2.54780269));

    return NULL;
}

static char *test_ciede2()
{
    const LABColor color_1 = {.l = 87.156334, .a = -12.049645, .b = -1.205325};
    const LABColor color_2 = {.l = 83.455727, .a = -9.040445, .b = -8.894289};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 4.22714281", close_enough(de00, 4.22714281));

    return NULL;
}

static char *test_ciede3()
{
    const LABColor color_1 = {.l = 79.718491, .a = 9.109915, .b = 13.727915};
    const LABColor color_2 = {.l = 78.717224, .a = 7.526546, .b = 5.597448};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 4.26012468", close_enough(de00, 4.26012468));

    return NULL;
}

static char *test_ciede4()
{
    const LABColor color_1 = {.l = 99.205299, .a = -3.339410, .b = 1.205873};
    const LABColor color_2 = {.l = 97.991730, .a = -2.497345, .b = 2.473533};

    const float de00 = ciede2000(color_1, color_2, default_ksub);
    mu_assert("de00 for this input should be 1.26915979", close_enough(de00, 1.26915979));

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_ciede);
    mu_run_test(test_ciede2);
    mu_run_test(test_ciede3);
    mu_run_test(test_ciede4);
    mu_run_test(test_ciede_scale_chroma_422_8b);
    mu_run_test(test_ciede_scale_chroma_422_16b);
    return NULL;
}
