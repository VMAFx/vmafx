/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

/*
 * Port of Netflix/vmaf 0497a0f29 test_colorspace.c (ADR-1822): the cases are
 * upstream's; the source colour is passed to
 * vmaf_picture_convert_context_init_with_color() because VmafPicture has no
 * colour field here, and the "mismatched colour" case becomes a mismatched
 * bit depth / format case since the context cannot see a source colour at
 * convert time.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/picture.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

static VmafColor bt709_color(void)
{
    VmafColor color = {
        .range = VMAF_COLOR_RANGE_LIMITED,
        .primaries = VMAF_COLOR_PRIMARIES_BT709,
        .trc = VMAF_COLOR_TRC_BT709,
        .matrix = VMAF_COLOR_MATRIX_BT709,
    };
    return color;
}

static int alloc_flat_picture(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned w,
                              unsigned h, uint8_t y_val, uint8_t c_val)
{
    int err = vmaf_picture_alloc(pic, pix_fmt, 8, w, h);
    if (err)
        return err;
    for (unsigned i = 0; i < 3; i++) {
        uint8_t val = i == 0 ? y_val : c_val;
        uint8_t *data = pic->data[i];
        for (unsigned j = 0; j < pic->h[i]; j++)
            memset(data + j * pic->stride[i], val, pic->w[i]);
    }
    return 0;
}

static int plane_is_near(const VmafPicture *pic, unsigned plane, uint8_t expected)
{
    const uint8_t *data = pic->data[plane];
    for (unsigned j = 0; j < pic->h[plane]; j++) {
        for (unsigned k = 0; k < pic->w[plane]; k++) {
            int diff = (int)data[j * pic->stride[plane] + k] - (int)expected;
            if (diff < -1 || diff > 1)
                return 0;
        }
    }
    return 1;
}

static char *test_colorspace_init_rejects_null_arguments(void)
{
    VmafPicture src;
    int err = alloc_flat_picture(&src, VMAF_PIX_FMT_YUV420P, 16, 16, 126, 128);
    mu_assert("problem during picture allocation", !err);
    VmafColor src_color = bt709_color();
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = bt709_color(),
    };
    VmafPictureConvertContext *ctx = NULL;

    err = vmaf_picture_convert_context_init_with_color(NULL, &src, &src_color, &target);
    mu_assert("NULL ctx should be rejected", err == -EINVAL);
    err = vmaf_picture_convert_context_init_with_color(&ctx, NULL, &src_color, &target);
    mu_assert("NULL src should be rejected", err == -EINVAL);
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, NULL, &target);
    mu_assert("NULL src_color should be rejected", err == -EINVAL);
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, NULL);
    mu_assert("NULL target should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);
    return NULL;
}

static char *test_colorspace_init_requires_known_color_metadata(void)
{
    VmafPicture src;
    int err = vmaf_picture_alloc(&src, VMAF_PIX_FMT_YUV420P, 8, 16, 16);
    mu_assert("problem during vmaf_picture_alloc", !err);

    /* an all-zero colour is the "unknown" description */
    VmafColor src_color;
    memset(&src_color, 0, sizeof(src_color));
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = bt709_color(),
    };
    VmafPictureConvertContext *ctx = NULL;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("init should fail with unspecified source color metadata", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);

    return NULL;
}

static char *test_colorspace_init_requires_known_target_color_metadata(void)
{
    VmafPicture src;
    int err = alloc_flat_picture(&src, VMAF_PIX_FMT_YUV420P, 16, 16, 126, 128);
    mu_assert("problem during picture allocation", !err);
    VmafColor src_color = bt709_color();

    /* target.color is left zero-initialized, i.e. not fully specified */
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
    };
    VmafPictureConvertContext *ctx = NULL;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("init should fail with unspecified target color metadata", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    /* a partially specified target is rejected too */
    target.color.range = VMAF_COLOR_RANGE_LIMITED;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("init should fail with partially specified target color", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);

    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) - one case per unsupported colour value and bit depth keeps the failing assertion identifiable; splitting hides it.
static char *test_colorspace_init_rejects_unsupported_values(void)
{
    VmafPicture src;
    int err = alloc_flat_picture(&src, VMAF_PIX_FMT_YUV420P, 16, 16, 126, 128);
    mu_assert("problem during picture allocation", !err);

    VmafColor src_color = bt709_color();
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = src_color,
    };
    VmafPictureConvertContext *ctx = NULL;

    /* values that are specified but not part of the supported set */
    target.color.matrix = (enum VmafColorMatrixCoefficients)99;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("unsupported target matrix should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    target.color = src_color;
    target.color.trc = (enum VmafColorTransferCharacteristic)99;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("unsupported target trc should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    target.color = src_color;
    target.color.primaries = (enum VmafColorPrimaries)99;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("unsupported target primaries should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    target.color = src_color;
    target.color.range = (enum VmafColorRange)99;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("unsupported target range should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    target.color = src_color;
    src_color.matrix = (enum VmafColorMatrixCoefficients)99;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("unsupported source matrix should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    src_color = bt709_color();
    target.bpc = 7;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("target bit depth below 8 should be rejected", err == -EINVAL);
    target.bpc = 17;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("target bit depth above 16 should be rejected", err == -EINVAL);
    mu_assert("no context should be created on failure", !ctx);

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);

    return NULL;
}

static char *test_colorspace_close_requires_context(void)
{
    int err = vmaf_picture_convert_context_close(NULL);
    mu_assert("closing a NULL context should fail", err == -EINVAL);

    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) - one conversion, every plane checked in place; splitting hides the assertion that fired.
static char *test_colorspace_convert_yuv420_to_yuv444(void)
{
    const unsigned w = 16;
    const unsigned h = 16;
    const uint8_t y_val = 126;
    const uint8_t c_val = 128;

    VmafPicture src;
    int err = alloc_flat_picture(&src, VMAF_PIX_FMT_YUV420P, w, h, y_val, c_val);
    mu_assert("problem during picture allocation", !err);

    VmafColor src_color = bt709_color();
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = bt709_color(),
    };
    VmafPictureConvertContext *ctx;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("problem during vmaf_picture_convert_context_init_with_color", !err);

    VmafPicture dst;
    err = vmaf_picture_convert(ctx, &dst, &src);
    mu_assert("problem during vmaf_picture_convert", !err);
    mu_assert("dst should be VMAF_PIX_FMT_YUV444P", dst.pix_fmt == VMAF_PIX_FMT_YUV444P);
    mu_assert("dst dimensions should match src", dst.w[0] == w && dst.h[0] == h);
    mu_assert("dst chroma planes should be full resolution for 444",
              dst.w[1] == w && dst.h[1] == h);

    /* A flat input frame should remain (near) flat after a colorspace- and
     * chroma-subsampling-only conversion; allow a small rounding tolerance. */
    mu_assert("converted flat luma plane should stay (near) flat", plane_is_near(&dst, 0, y_val));
    mu_assert("converted flat chroma plane should stay (near) flat",
              plane_is_near(&dst, 1, c_val) && plane_is_near(&dst, 2, c_val));

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&dst);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_convert_context_close(ctx);
    mu_assert("problem during vmaf_picture_convert_context_close", !err);

    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) - one conversion, every plane checked in place; splitting hides the assertion that fired.
static char *test_colorspace_convert_with_scaling(void)
{
    const unsigned src_w = 32;
    const unsigned src_h = 32;
    const unsigned dst_w = 16;
    const unsigned dst_h = 16;
    const uint8_t y_val = 126;
    const uint8_t c_val = 128;

    VmafPicture src;
    int err = alloc_flat_picture(&src, VMAF_PIX_FMT_YUV420P, src_w, src_h, y_val, c_val);
    mu_assert("problem during picture allocation", !err);

    VmafColor src_color = bt709_color();
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV420P,
        .bpc = 8,
        .w = dst_w,
        .h = dst_h,
        .color = bt709_color(),
        .resample_filter = VMAF_RESAMPLE_LANCZOS,
    };
    VmafPictureConvertContext *ctx;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("problem during vmaf_picture_convert_context_init_with_color", !err);

    VmafPicture dst;
    err = vmaf_picture_convert(ctx, &dst, &src);
    mu_assert("problem during vmaf_picture_convert", !err);
    mu_assert("dst should be downscaled to the requested dimensions",
              dst.w[0] == dst_w && dst.h[0] == dst_h);
    mu_assert("dst chroma planes should be subsampled accordingly",
              dst.w[1] == dst_w / 2 && dst.h[1] == dst_h / 2);

    /* A flat input frame should remain (near) flat after downscaling;
     * allow a small rounding tolerance. */
    mu_assert("downscaled flat luma plane should stay (near) flat", plane_is_near(&dst, 0, y_val));
    mu_assert("downscaled flat chroma plane should stay (near) flat",
              plane_is_near(&dst, 1, c_val) && plane_is_near(&dst, 2, c_val));

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&dst);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_convert_context_close(ctx);
    mu_assert("problem during vmaf_picture_convert_context_close", !err);

    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) - one conversion, every plane checked in place; splitting hides the assertion that fired.
static char *test_colorspace_context_converts_multiple_pictures(void)
{
    const unsigned w = 16;
    const unsigned h = 16;

    VmafPicture first;
    VmafPicture second;
    int err = alloc_flat_picture(&first, VMAF_PIX_FMT_YUV420P, w, h, 100, 128);
    mu_assert("problem during picture allocation", !err);
    err = alloc_flat_picture(&second, VMAF_PIX_FMT_YUV420P, w, h, 180, 128);
    mu_assert("problem during picture allocation", !err);

    VmafColor src_color = bt709_color();
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = bt709_color(),
    };
    VmafPictureConvertContext *ctx;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &first, &src_color, &target);
    mu_assert("problem during vmaf_picture_convert_context_init_with_color", !err);

    VmafPicture dst_first;
    VmafPicture dst_second;
    err = vmaf_picture_convert(ctx, &dst_first, &first);
    mu_assert("problem converting the first picture", !err);
    err = vmaf_picture_convert(ctx, &dst_second, &second);
    mu_assert("problem converting the second picture", !err);
    mu_assert("each conversion should produce its own picture",
              dst_first.data[0] != dst_second.data[0]);
    mu_assert("first converted picture should keep its own content",
              plane_is_near(&dst_first, 0, 100));
    mu_assert("second converted picture should keep its own content",
              plane_is_near(&dst_second, 0, 180));

    err = vmaf_picture_unref(&first);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&second);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&dst_first);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&dst_second);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_convert_context_close(ctx);
    mu_assert("problem during vmaf_picture_convert_context_close", !err);

    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) - one conversion, every plane checked in place; splitting hides the assertion that fired.
static char *test_colorspace_convert_rejects_mismatched_source(void)
{
    VmafPicture src;
    VmafPicture other_size;
    VmafPicture other_fmt;
    int err = alloc_flat_picture(&src, VMAF_PIX_FMT_YUV420P, 16, 16, 126, 128);
    mu_assert("problem during picture allocation", !err);
    err = alloc_flat_picture(&other_size, VMAF_PIX_FMT_YUV420P, 32, 32, 126, 128);
    mu_assert("problem during picture allocation", !err);
    err = alloc_flat_picture(&other_fmt, VMAF_PIX_FMT_YUV444P, 16, 16, 126, 128);
    mu_assert("problem during picture allocation", !err);

    VmafColor src_color = bt709_color();
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = bt709_color(),
    };
    VmafPictureConvertContext *ctx;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &src_color, &target);
    mu_assert("problem during vmaf_picture_convert_context_init_with_color", !err);

    VmafPicture dst;
    memset(&dst, 0, sizeof(dst));
    err = vmaf_picture_convert(ctx, &dst, &other_size);
    mu_assert("a source with different dimensions should be rejected", err == -EINVAL);
    mu_assert("dst should not be allocated on failure", !dst.ref);
    err = vmaf_picture_convert(ctx, &dst, &other_fmt);
    mu_assert("a source with a different pixel format should be rejected", err == -EINVAL);
    mu_assert("dst should not be allocated on failure", !dst.ref);
    err = vmaf_picture_convert(NULL, &dst, &src);
    mu_assert("a NULL context should be rejected", err == -EINVAL);

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&other_size);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_unref(&other_fmt);
    mu_assert("problem during vmaf_picture_unref", !err);
    err = vmaf_picture_convert_context_close(ctx);
    mu_assert("problem during vmaf_picture_convert_context_close", !err);

    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_colorspace_init_rejects_null_arguments),
        MU_TEST(test_colorspace_init_requires_known_color_metadata),
        MU_TEST(test_colorspace_init_requires_known_target_color_metadata),
        MU_TEST(test_colorspace_init_rejects_unsupported_values),
        MU_TEST(test_colorspace_close_requires_context),
        MU_TEST(test_colorspace_convert_yuv420_to_yuv444),
        MU_TEST(test_colorspace_convert_with_scaling),
        MU_TEST(test_colorspace_context_converts_multiple_pictures),
        MU_TEST(test_colorspace_convert_rejects_mismatched_source),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
