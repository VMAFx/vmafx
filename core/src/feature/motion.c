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

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "offset.h"
#include "motion_options.h"
#include "mem.h"
#include "common/alignment.h"
#include "common/convolution.h"
#include "common/convolution_internal.h"
#include "motion_tools.h"

#define convolution_f32_c convolution_f32_c_s
#define FILTER_3 FILTER_3_s
#define FILTER_5 FILTER_5_s
#define FILTER_5_NO_OP FILTER_5_NO_OP_s
#define offset_image offset_image_s

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* Bilinear downscaler for motion_add_scale1 support.
 * Mirrors the vif_scale_frame_bilinear_s logic from vif_tools.c; kept here
 * as a private static to avoid depending on the VIF module (which is
 * deferred per Research-0024). */
static float motion_mirror_f(float i, float left, float right)
{
    return (i < left ? -i : i > right ? 2.0f * right - i : i);
}

static float motion_bilinear_interp(const float *src, int width, int height, int src_stride,
                                    float x, float y)
{
    int x1 = (int)motion_mirror_f(floorf(x), 0.0f, (float)(width - 1));
    int x2 = (int)motion_mirror_f(ceilf(x), 0.0f, (float)(width - 1));
    int y1 = (int)motion_mirror_f(floorf(y), 0.0f, (float)(height - 1));
    int y2 = (int)motion_mirror_f(ceilf(y), 0.0f, (float)(height - 1));

    float dx = x - (float)x1;
    float dy = y - (float)y1;

    return ((1.0f - dy) * (1.0f - dx) * src[y1 * src_stride + x1] +
            (1.0f - dy) * dx * src[y1 * src_stride + x2] +
            dy * (1.0f - dx) * src[y2 * src_stride + x1] + dy * dx * src[y2 * src_stride + x2]);
}

static void motion_scale_bilinear(const float *src, float *dst, int src_w, int src_h,
                                  int src_stride, int dst_w, int dst_h, int dst_stride)
{
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)dst_h * sizeof(float));
        return;
    }

    float ratio_x = (float)src_w / (float)dst_w;
    float ratio_y = (float)src_h / (float)dst_h;

    for (int y = 0; y < dst_h; y++) {
        float yy = (y + 0.5f) * ratio_y - 0.5f;
        for (int x = 0; x < dst_w; x++) {
            float xx = (x + 0.5f) * ratio_x - 0.5f;
            dst[y * dst_stride + x] = motion_bilinear_interp(src, src_w, src_h, src_stride, xx, yy);
        }
    }
}

/**
 * Note: img1_stride and img2_stride are in terms of (sizeof(float) bytes)
 */
float vmaf_image_sad_c(const float *img1, const float *img2, int width, int height, int img1_stride,
                       int img2_stride, int motion_add_scale1)
{
    float motion_scale0 = 0.0;
    float accum = (float)0.0;

    for (int i = 0; i < height; ++i) {
        float accum_line = (float)0.0;
        for (int j = 0; j < width; ++j) {
            float img1px = img1[i * img1_stride + j];
            float img2px = img2[i * img2_stride + j];
            accum_line += fabsf(img1px - img2px);
        }
        accum += accum_line;
    }
    motion_scale0 = (float)(accum / (width * height));

    if (motion_add_scale1 == 1) {
        float motion_scale1 = 0.0;
        float accum_scale1 = (float)0.0;
        int scaled_width = (int)(width * 0.5 + 0.5);
        int scaled_height = (int)(height * 0.5 + 0.5);
        int scaled_float_stride = ALIGN_CEIL(scaled_width * sizeof(float));
        float *img1_scaled = aligned_malloc((size_t)scaled_float_stride * scaled_height, 32);
        float *img2_scaled = aligned_malloc((size_t)scaled_float_stride * scaled_height, 32);
        /* Guard OOM: aligned_malloc may return NULL. */
        if (!img1_scaled || !img2_scaled) {
            aligned_free(img1_scaled);
            aligned_free(img2_scaled);
            return motion_scale0;
        }

        /* Pass the actual caller-supplied strides (already in element units,
         * per the function contract at line 90) rather than recomputing from
         * width.  The caller's strides may include alignment padding that
         * ALIGN_CEIL(width*sizeof(float))/sizeof(float) does not match.    */
        motion_scale_bilinear(img1, img1_scaled, width, height, img1_stride, scaled_width,
                              scaled_height, scaled_float_stride / sizeof(float));
        motion_scale_bilinear(img2, img2_scaled, width, height, img2_stride, scaled_width,
                              scaled_height, scaled_float_stride / sizeof(float));

        for (int i = 0; i < scaled_height; ++i) {
            float accum_line = (float)0.0;
            for (int j = 0; j < scaled_width; ++j) {
                float img1px = img1_scaled[(size_t)i * scaled_float_stride / sizeof(float) + j];
                float img2px = img2_scaled[(size_t)i * scaled_float_stride / sizeof(float) + j];
                accum_line += fabsf(img1px - img2px);
            }
            accum_scale1 += accum_line;
        }

        aligned_free(img1_scaled);
        aligned_free(img2_scaled);

        motion_scale1 = (float)(accum_scale1 / (scaled_width * scaled_height));

        return motion_scale0 + motion_scale1;
    } else {
        return motion_scale0;
    }
}

/**
 * Note: ref_stride and dis_stride are in terms of bytes
 */
int compute_motion(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride,
                   double *score, int motion_decimate)
{
    if (ref_stride % sizeof(float) != 0) {
        printf("error: ref_stride %% sizeof(float) != 0, ref_stride = %d, sizeof(float) = %zu.\n",
               ref_stride, sizeof(float));
        (void)fflush(stdout);
        goto fail;
    }
    if (dis_stride % sizeof(float) != 0) {
        printf("error: dis_stride %% sizeof(float) != 0, dis_stride = %d, sizeof(float) = %zu.\n",
               dis_stride, sizeof(float));
        (void)fflush(stdout);
        goto fail;
    }
    // stride for vmaf_image_sad_c is in terms of (sizeof(float) bytes)
    *score = vmaf_image_sad_c(ref, dis, w, h, ref_stride / sizeof(float),
                              dis_stride / sizeof(float), motion_decimate);

    return 0;

fail:
    return 1;
}
