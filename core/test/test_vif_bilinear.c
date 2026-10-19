/**
 * Copyright 2016-2026 Netflix, Inc.
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
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

/*
 * Bilinear scaling with a column table (Netflix/vmaf 78e11b52c) against the
 * per-pixel form it replaced. Both vif_scale_frame_s(vif_scale_bilinear, ...)
 * (which walks the columns in chunks of 1024) and the precomputed path
 * SpEED uses (vif_scale_frame_bilinear_precompute_columns_s() +
 * vif_scale_frame_bilinear_precomputed_s()) must return the bits of
 * reference_bilinear() below, which is the per-pixel code verbatim: on
 * downscales and upscales, odd sizes, widths across one, two and four column
 * chunks, padded strides and a same-size pass-through.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "feature/vif_tools.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* The per-pixel bilinear scaler of vif_tools.c before 78e11b52c. */
static float ref_mirror(float i, float left, float right)
{
    return (i < left ? -i : i > right ? 2 * right - i : i);
}

static float ref_interpolate(const float *src, int width, int height, int src_stride, float x,
                             float y)
{
    int x1 = (int)ref_mirror(floorf(x), 0, (float)(width - 1));
    int x2 = (int)ref_mirror(ceilf(x), 0, (float)(width - 1));
    int y1 = (int)ref_mirror(floorf(y), 0, (float)(height - 1));
    int y2 = (int)ref_mirror(ceilf(y), 0, (float)(height - 1));

    float dx = x - x1;
    float dy = y - y1;

    return ((1 - dy) * (1 - dx) * src[y1 * src_stride + x1] +
            (1 - dy) * dx * src[y1 * src_stride + x2] + dy * (1 - dx) * src[y2 * src_stride + x1] +
            dy * dx * src[y2 * src_stride + x2]);
}

static void reference_bilinear(const float *src, float *dst, int src_w, int src_h, int src_stride,
                               int dst_w, int dst_h, int dst_stride)
{
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)dst_h * sizeof(float));
        return;
    }
    float ratio_x = (float)src_w / dst_w;
    float ratio_y = (float)src_h / dst_h;
    for (int y = 0; y < dst_h; y++) {
        float yy = (float)((y + 0.5) * ratio_y - 0.5);
        for (int x = 0; x < dst_w; x++) {
            float xx = (float)((x + 0.5) * ratio_x - 0.5);
            dst[y * dst_stride + x] = ref_interpolate(src, src_w, src_h, src_stride, xx, yy);
        }
    }
}

typedef struct BilinearCase {
    int src_w, src_h, dst_w, dst_h, pad;
} BilinearCase;

static const BilinearCase cases[] = {
    {576, 324, 288, 162, 0},     /* SpEED 0.5 on the Netflix pair */
    {288, 162, 173, 97, 3},      /* 0.6, odd result */
    {1920, 1080, 960, 540, 0},   /* one chunk */
    {1920, 1080, 2880, 1620, 7}, /* three chunks, upscale */
    {1920, 1080, 3840, 2160, 0}, /* four chunks */
    {255, 97, 383, 146, 1},      /* odd sizes, upscale */
    {17, 31, 8, 15, 0},          /* small */
    {1000, 7, 1025, 3, 5},       /* one column past a chunk */
    {2049, 5, 2048, 5, 0},       /* near identity, two chunks */
    {64, 64, 64, 64, 0},         /* same size: pass-through */
};

static uint32_t lcg_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void fill_plane(float *p, size_t n, uint32_t seed)
{
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++)
        p[i] = (float)(lcg_next(&s) >> 8) * (255.0f / 16777216.0f) - 128.0f;
}

typedef struct BilinearBuffers {
    float *src, *ref, *out;
    int *x1a, *x2a;
    float *dxa;
} BilinearBuffers;

static void free_buffers(BilinearBuffers *b)
{
    free(b->src);
    free(b->ref);
    free(b->out);
    free(b->x1a);
    free(b->x2a);
    free(b->dxa);
    memset(b, 0, sizeof(*b));
}

static int alloc_buffers(BilinearBuffers *b, const BilinearCase *c, size_t dst_n)
{
    const size_t src_n = (size_t)(c->src_w + c->pad) * (size_t)c->src_h;
    b->src = malloc(src_n * sizeof(float));
    b->ref = malloc(dst_n * sizeof(float));
    b->out = malloc(dst_n * sizeof(float));
    b->x1a = malloc((size_t)c->dst_w * sizeof(int));
    b->x2a = malloc((size_t)c->dst_w * sizeof(int));
    b->dxa = malloc((size_t)c->dst_w * sizeof(float));
    if (!b->src || !b->ref || !b->out || !b->x1a || !b->x2a || !b->dxa)
        return -1;
    fill_plane(b->src, src_n, (uint32_t)(c->src_w * 31 + c->dst_w));
    return 0;
}

/* Both new paths of one case against the reference, bit for bit. */
static char *check_case(const BilinearCase *c)
{
    const int src_stride = c->src_w + c->pad;
    const int dst_stride = c->dst_w + c->pad;
    const size_t dst_n = (size_t)dst_stride * (size_t)c->dst_h;
    BilinearBuffers b = {0};
    if (alloc_buffers(&b, c, dst_n)) {
        free_buffers(&b);
        return "out of memory";
    }
    memset(b.ref, 0, dst_n * sizeof(float));
    memset(b.out, 0, dst_n * sizeof(float));
    reference_bilinear(b.src, b.ref, c->src_w, c->src_h, src_stride, c->dst_w, c->dst_h,
                       dst_stride);

    vif_scale_frame_s(vif_scale_bilinear, b.src, b.out, c->src_w, c->src_h, src_stride, c->dst_w,
                      c->dst_h, dst_stride);
    const int chunked_ok = memcmp(b.ref, b.out, dst_n * sizeof(float)) == 0;

    memset(b.out, 0, dst_n * sizeof(float));
    vif_scale_frame_bilinear_precompute_columns_s(c->src_w, c->dst_w, b.x1a, b.x2a, b.dxa);
    vif_scale_frame_bilinear_precomputed_s(b.src, b.out, c->src_w, c->src_h, src_stride, c->dst_w,
                                           c->dst_h, dst_stride, b.x1a, b.x2a, b.dxa);
    const int precomputed_ok = memcmp(b.ref, b.out, dst_n * sizeof(float)) == 0;

    free_buffers(&b);
    mu_assert("vif_scale_frame_s(bilinear) differs from the per-pixel scaler", chunked_ok);
    mu_assert("the precomputed bilinear path differs from the per-pixel scaler", precomputed_ok);
    return NULL;
}

static char *test_bilinear_columns(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        mu_assert_msg(check_case(&cases[i]));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_bilinear_columns);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
