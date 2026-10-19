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

#include <errno.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "libvmaf/picture.h"
#include "conversion_policy.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

static const VmafColor bt709_color = {
    .range = VMAF_COLOR_RANGE_LIMITED,
    .primaries = VMAF_COLOR_PRIMARIES_BT709,
    .trc = VMAF_COLOR_TRC_BT709,
    .matrix = VMAF_COLOR_MATRIX_BT709,
};

static const VmafColor pq_bt2020nc_color = {
    .range = VMAF_COLOR_RANGE_LIMITED,
    .primaries = VMAF_COLOR_PRIMARIES_BT2020,
    .trc = VMAF_COLOR_TRC_SMPTE2084,
    .matrix = VMAF_COLOR_MATRIX_BT2020_NCL,
};

static const VmafColor pq_ictcp_color = {
    .range = VMAF_COLOR_RANGE_LIMITED,
    .primaries = VMAF_COLOR_PRIMARIES_BT2020,
    .trc = VMAF_COLOR_TRC_SMPTE2084,
    .matrix = VMAF_COLOR_MATRIX_ICTCP,
};

/* Fork adaptation (ADR-1822): VmafPicture has no colour member, so a test
 * picture is the picture plus the colour the caller declares for it. */
typedef struct {
    VmafPicture pic;
    VmafColor color;
} TaggedPic;

static TaggedPic make_pic(const VmafColor *color, enum VmafPixelFormat pix_fmt, unsigned bpc)
{
    TaggedPic tagged;
    memset(&tagged, 0, sizeof(tagged));
    tagged.color = *color;
    tagged.pic.pix_fmt = pix_fmt;
    tagged.pic.bpc = bpc;
    return tagged;
}

static int policy_target(const TaggedPic *ref, const TaggedPic *dist,
                         const VmafPictureConvertTarget *model_target, bool *needs_conversion,
                         VmafPictureConvertTarget *target)
{
    return vmaf_conversion_policy_target(ref ? &ref->pic : NULL, ref ? &ref->color : NULL,
                                         dist ? &dist->pic : NULL, dist ? &dist->color : NULL,
                                         model_target, needs_conversion, target);
}

/* a model target that pins only the colorspace */
static VmafPictureConvertTarget color_target(const VmafColor *color)
{
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));
    target.color = *color;
    return target;
}

static char *test_no_model_target_is_pass_through(void)
{
    /* a model without a target never converts, however pictures are tagged */
    const VmafColor unset = {0};
    const VmafColor *colors[] = {&unset, &bt709_color, &pq_bt2020nc_color};
    for (unsigned i = 0; i < sizeof(colors) / sizeof(*colors); i++) {
        TaggedPic pic = make_pic(colors[i], VMAF_PIX_FMT_YUV420P, 10);
        bool needs_conversion = true;
        VmafPictureConvertTarget target;
        memset(&target, 0, sizeof(target));
        int err = policy_target(&pic, &pic, NULL, &needs_conversion, &target);
        mu_assert("policy should accept any color without a model target", !err);
        mu_assert("no model target should not need conversion", !needs_conversion);
        mu_assert("target should be left untouched", target.color.trc == VMAF_COLOR_TRC_UNKNOWN);
    }

    return NULL;
}

static char *test_target_picks_model_colorspace(void)
{
    TaggedPic pic = make_pic(&pq_bt2020nc_color, VMAF_PIX_FMT_YUV420P, 10);
    const VmafPictureConvertTarget model = color_target(&pq_ictcp_color);
    bool needs_conversion = false;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));
    int err = policy_target(&pic, &pic, &model, &needs_conversion, &target);
    mu_assert("policy should accept fully specified colors", !err);
    mu_assert("differing source and target should need conversion", needs_conversion);
    mu_assert("target should be the model's colorspace",
              vmaf_conversion_policy_color_equal(&target.color, &pq_ictcp_color));
    mu_assert("an unpinned pixel format should stay unknown",
              target.pix_fmt == VMAF_PIX_FMT_UNKNOWN);
    mu_assert("an unpinned bit depth should stay 0", target.bpc == 0);

    return NULL;
}

static char *test_source_already_matching_target_skips_conversion(void)
{
    TaggedPic pic = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 10);
    const VmafPictureConvertTarget model = color_target(&pq_ictcp_color);
    bool needs_conversion = true;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));
    int err = policy_target(&pic, &pic, &model, &needs_conversion, &target);
    mu_assert("policy should accept matching colors", !err);
    mu_assert("source already in the target should not need conversion", !needs_conversion);

    return NULL;
}

/* a model target that pins 4:2:0 at 16 bits */
static VmafPictureConvertTarget pinned_target(void)
{
    VmafPictureConvertTarget model = color_target(&pq_ictcp_color);
    model.pix_fmt = VMAF_PIX_FMT_YUV420P;
    model.bpc = 16;
    return model;
}

static char *test_pinned_bit_depth_is_part_of_the_target(void)
{
    const VmafPictureConvertTarget model = pinned_target();
    bool needs_conversion = false;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));

    /* right colorspace, wrong bit depth */
    TaggedPic ten_bit = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 10);
    int err = policy_target(&ten_bit, &ten_bit, &model, &needs_conversion, &target);
    mu_assert("policy should accept the pictures", !err);
    mu_assert("a different bit depth should need conversion", needs_conversion);
    mu_assert("the pinned pixel format should be in the target",
              target.pix_fmt == VMAF_PIX_FMT_YUV420P);
    mu_assert("the pinned bit depth should be in the target", target.bpc == 16);

    return NULL;
}

static char *test_pinned_pixel_format_is_part_of_the_target(void)
{
    const VmafPictureConvertTarget model = pinned_target();
    bool needs_conversion = false;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));

    /* right colorspace, wrong chroma subsampling */
    TaggedPic yuv444 = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV444P, 16);
    int err = policy_target(&yuv444, &yuv444, &model, &needs_conversion, &target);
    mu_assert("policy should accept the pictures", !err);
    mu_assert("a different pixel format should need conversion", needs_conversion);

    return NULL;
}

static char *test_pinned_target_matching_pictures_are_not_converted(void)
{
    const VmafPictureConvertTarget model = pinned_target();
    bool needs_conversion = true;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));

    /* everything matches */
    TaggedPic exact = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 16);
    int err = policy_target(&exact, &exact, &model, &needs_conversion, &target);
    mu_assert("policy should accept the pictures", !err);
    mu_assert("pictures matching colorspace, format and depth should not "
              "need conversion",
              !needs_conversion);

    return NULL;
}

static char *test_unpinned_format_and_depth_follow_the_source(void)
{
    const VmafPictureConvertTarget model = color_target(&pq_ictcp_color);
    bool needs_conversion = false;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));
    const struct {
        enum VmafPixelFormat fmt;
        unsigned bpc;
    } formats[] = {
        {VMAF_PIX_FMT_YUV420P, 8},
        {VMAF_PIX_FMT_YUV444P, 12},
        {VMAF_PIX_FMT_YUV422P, 16},
    };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(*formats); i++) {
        TaggedPic pic = make_pic(&pq_ictcp_color, formats[i].fmt, formats[i].bpc);
        int err = policy_target(&pic, &pic, &model, &needs_conversion, &target);
        mu_assert("policy should accept the pictures", !err);
        mu_assert("a target that pins neither format nor depth should accept "
                  "any",
                  !needs_conversion);
    }

    return NULL;
}

static char *test_one_side_differing_still_needs_conversion(void)
{
    TaggedPic matching = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 10);
    TaggedPic other = make_pic(&pq_bt2020nc_color, VMAF_PIX_FMT_YUV420P, 10);
    const VmafPictureConvertTarget model = color_target(&pq_ictcp_color);
    bool needs_conversion = false;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));
    int err = policy_target(&matching, &other, &model, &needs_conversion, &target);
    mu_assert("policy should accept fully specified colors", !err);
    mu_assert("one differing picture should need conversion", needs_conversion);

    return NULL;
}

static char *test_ref_and_dist_may_differ_from_each_other(void)
{
    TaggedPic sdr = make_pic(&bt709_color, VMAF_PIX_FMT_YUV420P, 10);
    TaggedPic hdr = make_pic(&pq_bt2020nc_color, VMAF_PIX_FMT_YUV420P, 10);
    const VmafPictureConvertTarget model = color_target(&pq_ictcp_color);
    bool needs_conversion = false;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));
    int err = policy_target(&sdr, &hdr, &model, &needs_conversion, &target);
    mu_assert("each picture converts to the shared target, so differing "
              "sources are not an error",
              !err);
    mu_assert("differing sources should need conversion", needs_conversion);

    return NULL;
}

static char *test_unspecified_source_with_target_is_an_error(void)
{
    const VmafColor unset = {0};
    VmafColor partial_color = pq_bt2020nc_color;
    partial_color.range = VMAF_COLOR_RANGE_UNKNOWN;

    TaggedPic none = make_pic(&unset, VMAF_PIX_FMT_YUV420P, 10);
    TaggedPic full = make_pic(&pq_bt2020nc_color, VMAF_PIX_FMT_YUV420P, 10);
    TaggedPic partial = make_pic(&partial_color, VMAF_PIX_FMT_YUV420P, 10);
    const VmafPictureConvertTarget model = color_target(&pq_ictcp_color);

    const struct {
        const TaggedPic *ref, *dist;
    } cases[] = {
        {&none, &none}, {&none, &full}, {&full, &none}, {&partial, &full}, {&full, &partial},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        bool needs_conversion = true;
        VmafPictureConvertTarget target;
        memset(&target, 0, sizeof(target));
        int err = policy_target(cases[i].ref, cases[i].dist, &model, &needs_conversion, &target);
        mu_assert("an unspecified or partly specified source should be "
                  "rejected when the model has a target",
                  err == -EINVAL);
    }

    return NULL;
}

static char *test_target_equal_compares_format_and_depth(void)
{
    VmafPictureConvertTarget a = color_target(&pq_ictcp_color);
    VmafPictureConvertTarget b = color_target(&pq_ictcp_color);
    mu_assert("identical targets should be equal", vmaf_conversion_policy_target_equal(&a, &b));
    b.bpc = 16;
    mu_assert("targets with different bit depth should differ",
              !vmaf_conversion_policy_target_equal(&a, &b));
    b.bpc = 0;
    b.pix_fmt = VMAF_PIX_FMT_YUV420P;
    mu_assert("targets with different pixel format should differ",
              !vmaf_conversion_policy_target_equal(&a, &b));
    b.pix_fmt = VMAF_PIX_FMT_UNKNOWN;
    b.color = bt709_color;
    mu_assert("targets with different color should differ",
              !vmaf_conversion_policy_target_equal(&a, &b));

    return NULL;
}

static char *test_null_arguments_are_rejected(void)
{
    TaggedPic pic = make_pic(&bt709_color, VMAF_PIX_FMT_YUV420P, 10);
    bool needs_conversion;
    VmafPictureConvertTarget target;
    memset(&target, 0, sizeof(target));

    mu_assert("null ref should be rejected",
              policy_target(NULL, &pic, NULL, &needs_conversion, &target) == -EINVAL);
    mu_assert("null dist should be rejected",
              policy_target(&pic, NULL, NULL, &needs_conversion, &target) == -EINVAL);
    mu_assert("null needs_conversion should be rejected",
              policy_target(&pic, &pic, NULL, NULL, &target) == -EINVAL);
    mu_assert("null color should be rejected",
              vmaf_conversion_policy_target(&pic.pic, NULL, &pic.pic, &pic.color, NULL,
                                            &needs_conversion, &target) == -EINVAL);
    mu_assert("null target should be rejected",
              policy_target(&pic, &pic, NULL, &needs_conversion, NULL) == -EINVAL);

    return NULL;
}

/* scale a w x h pair to `display`, returning the policy's error */
static int scale(unsigned w, unsigned h, unsigned display_resolution, bool *needs_conversion,
                 VmafPictureConvertTarget *target)
{
    TaggedPic tagged = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 10);
    tagged.pic.w[0] = w;
    tagged.pic.h[0] = h;
    *target = color_target(&pq_ictcp_color);
    *needs_conversion = false;
    return vmaf_conversion_policy_scale(&tagged.pic, &tagged.pic, display_resolution,
                                        needs_conversion, target);
}

static char *test_scale_upscales_short_side_to_display(void)
{
    bool needs_conversion;
    VmafPictureConvertTarget target;
    mu_assert("720p to 1080 should be accepted",
              !scale(1280, 720, 1080, &needs_conversion, &target));
    mu_assert("720p should scale to 1920x1080", target.w == 1920 && target.h == 1080);
    mu_assert("upscaling should use bicubic", target.resample_filter == VMAF_RESAMPLE_BICUBIC);
    mu_assert("a resize should need conversion", needs_conversion);

    mu_assert("1080p to 2160 should be accepted",
              !scale(1920, 1080, 2160, &needs_conversion, &target));
    mu_assert("1080p should scale to 3840x2160", target.w == 3840 && target.h == 2160);

    return NULL;
}

static char *test_scale_downscales_short_side_to_display(void)
{
    bool needs_conversion;
    VmafPictureConvertTarget target;
    mu_assert("2160p to 1080 should be accepted",
              !scale(3840, 2160, 1080, &needs_conversion, &target));
    mu_assert("2160p should scale to 1920x1080", target.w == 1920 && target.h == 1080);
    mu_assert("downscaling should use lanczos", target.resample_filter == VMAF_RESAMPLE_LANCZOS);
    mu_assert("a resize should need conversion", needs_conversion);

    return NULL;
}

static char *test_scale_follows_source_orientation(void)
{
    bool needs_conversion;
    VmafPictureConvertTarget target;
    mu_assert("portrait 720x1280 to 1080 should be accepted",
              !scale(720, 1280, 1080, &needs_conversion, &target));
    mu_assert("portrait should scale to 1080x1920", target.w == 1080 && target.h == 1920);

    return NULL;
}

static char *test_scale_keeps_aspect_ratio_rounded_to_even(void)
{
    bool needs_conversion;
    VmafPictureConvertTarget target;
    /* 854 * 1080 / 480 = 1921.5 */
    mu_assert("854x480 to 1080 should be accepted",
              !scale(854, 480, 1080, &needs_conversion, &target));
    mu_assert("the long side should round to the nearest even",
              target.w == 1922 && target.h == 1080);
    /* 4:3 */
    mu_assert("1440x1080 to 2160 should be accepted",
              !scale(1440, 1080, 2160, &needs_conversion, &target));
    mu_assert("4:3 should keep its aspect ratio", target.w == 2880 && target.h == 2160);

    return NULL;
}

static char *test_scale_at_display_keeps_source_size(void)
{
    bool needs_conversion;
    VmafPictureConvertTarget target;
    mu_assert("1080p to 1080 should be accepted",
              !scale(1920, 1080, 1080, &needs_conversion, &target));
    mu_assert("a source at the display size should keep its size",
              target.w == 1920 && target.h == 1080);
    mu_assert("a source at the display size should not need conversion", !needs_conversion);

    /* an odd long side is kept, not rounded */
    mu_assert("1919x1080 to 1080 should be accepted",
              !scale(1919, 1080, 1080, &needs_conversion, &target));
    mu_assert("an odd long side at the display size should be kept",
              target.w == 1919 && target.h == 1080);

    return NULL;
}

static char *test_scale_rejects_extreme_aspect_ratios(void)
{
    bool needs_conversion;
    VmafPictureConvertTarget target;
    mu_assert("32:9 should be accepted", !scale(3840, 1080, 1080, &needs_conversion, &target));
    mu_assert("4:1 upscaled should be accepted",
              !scale(1440, 360, 1080, &needs_conversion, &target) && target.w == 4320 &&
                  target.h == 1080);
    mu_assert("wider than 4:1 should be rejected",
              scale(1442, 360, 1080, &needs_conversion, &target) == -EINVAL);
    mu_assert("an extreme portrait should be rejected",
              scale(8, 2000, 1080, &needs_conversion, &target) == -EINVAL);

    return NULL;
}

static char *test_scale_rejects_different_sizes(void)
{
    TaggedPic ref = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 10);
    TaggedPic dist = ref;
    ref.pic.w[0] = 1920;
    ref.pic.h[0] = 1080;
    dist.pic.w[0] = 1280;
    dist.pic.h[0] = 720;
    bool needs_conversion = false;
    VmafPictureConvertTarget target = color_target(&pq_ictcp_color);
    mu_assert("different reference and distorted sizes are not supported yet",
              vmaf_conversion_policy_scale(&ref.pic, &dist.pic, 1080, &needs_conversion, &target) ==
                  -EINVAL);
    mu_assert("no display should be rejected",
              vmaf_conversion_policy_scale(&ref.pic, &ref.pic, 0, &needs_conversion, &target) ==
                  -EINVAL);

    return NULL;
}

static char *test_picture_matches_compares_pinned_size(void)
{
    TaggedPic tagged = make_pic(&pq_ictcp_color, VMAF_PIX_FMT_YUV420P, 10);
    tagged.pic.w[0] = 1280;
    tagged.pic.h[0] = 720;
    VmafPictureConvertTarget target = color_target(&pq_ictcp_color);
    mu_assert("an unpinned size should match any picture",
              vmaf_conversion_policy_picture_matches(&tagged.pic, &tagged.color, &target));
    target.w = 1920;
    target.h = 1080;
    mu_assert("a picture of a different size should not match",
              !vmaf_conversion_policy_picture_matches(&tagged.pic, &tagged.color, &target));

    return NULL;
}

static const MuTest policy_tests[] = {
    MU_TEST(test_no_model_target_is_pass_through),
    MU_TEST(test_target_picks_model_colorspace),
    MU_TEST(test_source_already_matching_target_skips_conversion),
    MU_TEST(test_pinned_bit_depth_is_part_of_the_target),
    MU_TEST(test_pinned_pixel_format_is_part_of_the_target),
    MU_TEST(test_pinned_target_matching_pictures_are_not_converted),
    MU_TEST(test_unpinned_format_and_depth_follow_the_source),
    MU_TEST(test_one_side_differing_still_needs_conversion),
    MU_TEST(test_ref_and_dist_may_differ_from_each_other),
    MU_TEST(test_unspecified_source_with_target_is_an_error),
    MU_TEST(test_target_equal_compares_format_and_depth),
    MU_TEST(test_null_arguments_are_rejected),
    MU_TEST(test_scale_upscales_short_side_to_display),
    MU_TEST(test_scale_downscales_short_side_to_display),
    MU_TEST(test_scale_follows_source_orientation),
    MU_TEST(test_scale_keeps_aspect_ratio_rounded_to_even),
    MU_TEST(test_scale_at_display_keeps_source_size),
    MU_TEST(test_scale_rejects_extreme_aspect_ratios),
    MU_TEST(test_scale_rejects_different_sizes),
    MU_TEST(test_picture_matches_compares_pinned_size),
};

mu_message_t run_tests(void)
{
    return mu_run_table(policy_tests, MU_TABLE_LEN(policy_tests));
}

/* NOLINTEND(modernize-use-nullptr) */
