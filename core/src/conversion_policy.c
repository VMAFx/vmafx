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

#include "conversion_policy.h"
#include "log.h"

static bool color_is_fully_specified(const VmafColor *color)
{
    return color->range != VMAF_COLOR_RANGE_UNKNOWN &&
           color->primaries != VMAF_COLOR_PRIMARIES_UNKNOWN &&
           color->trc != VMAF_COLOR_TRC_UNKNOWN && color->matrix != VMAF_COLOR_MATRIX_UNKNOWN;
}

bool vmaf_conversion_policy_color_equal(const VmafColor *a, const VmafColor *b)
{
    return a->range == b->range && a->primaries == b->primaries && a->trc == b->trc &&
           a->matrix == b->matrix;
}

static void log_missing_color(const char *which, const VmafColor *color)
{
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "the model requires source colorimetry, but the %s picture has "
             "unspecified:%s%s%s%s. Set --color_range_ref/_dist, "
             "--color_primaries_ref/_dist, --color_trc_ref/_dist and "
             "--color_matrix_ref/_dist (CLI) or vmaf_set_input_colorimetry() "
             "(library).\n",
             which, color->range == VMAF_COLOR_RANGE_UNKNOWN ? " range" : "",
             color->primaries == VMAF_COLOR_PRIMARIES_UNKNOWN ? " primaries" : "",
             color->trc == VMAF_COLOR_TRC_UNKNOWN ? " trc" : "",
             color->matrix == VMAF_COLOR_MATRIX_UNKNOWN ? " matrix" : "");
}

bool vmaf_conversion_policy_picture_matches(const VmafPicture *pic, const VmafColor *pic_color,
                                            const VmafPictureConvertTarget *target)
{
    return vmaf_conversion_policy_color_equal(pic_color, &target->color) &&
           (!target->pix_fmt || pic->pix_fmt == target->pix_fmt) &&
           (!target->bpc || pic->bpc == target->bpc) && (!target->w || pic->w[0] == target->w) &&
           (!target->h || pic->h[0] == target->h);
}

bool vmaf_conversion_policy_target_equal(const VmafPictureConvertTarget *a,
                                         const VmafPictureConvertTarget *b)
{
    return vmaf_conversion_policy_color_equal(&a->color, &b->color) && a->pix_fmt == b->pix_fmt &&
           a->bpc == b->bpc;
}

int vmaf_conversion_policy_target(const VmafPicture *ref, const VmafColor *ref_color,
                                  const VmafPicture *dist, const VmafColor *dist_color,
                                  const VmafPictureConvertTarget *model_target,
                                  bool *needs_conversion, VmafPictureConvertTarget *target)
{
    if (!ref || !ref_color || !dist || !dist_color || !needs_conversion || !target)
        return -EINVAL;

    if (!model_target) {
        *needs_conversion = false;
        return 0;
    }

    if (!color_is_fully_specified(ref_color)) {
        log_missing_color("reference", ref_color);
        return -EINVAL;
    }
    if (!color_is_fully_specified(dist_color)) {
        log_missing_color("distorted", dist_color);
        return -EINVAL;
    }

    target->color = model_target->color;
    target->pix_fmt = model_target->pix_fmt;
    target->bpc = model_target->bpc;
    *needs_conversion = !vmaf_conversion_policy_picture_matches(ref, ref_color, target) ||
                        !vmaf_conversion_policy_picture_matches(dist, dist_color, target);
    return 0;
}

/* Both pictures must have the same, non-zero size: the scaled size is derived
 * from one of them. */
static int scale_check_sizes(const VmafPicture *ref, const VmafPicture *dist)
{
    const unsigned w = ref->w[0];
    const unsigned h = ref->h[0];
    if (dist->w[0] != w || dist->h[0] != h) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "the model scales its inputs, which requires the reference "
                 "(%ux%u) and distorted (%ux%u) pictures to be the same size\n",
                 w, h, dist->w[0], dist->h[0]);
        return -EINVAL;
    }
    if (!w || !h)
        return -EINVAL;
    return 0;
}

int vmaf_conversion_policy_scale(const VmafPicture *ref, const VmafPicture *dist,
                                 unsigned display_resolution, bool *needs_conversion,
                                 VmafPictureConvertTarget *target)
{
    if (!ref || !dist || !needs_conversion || !target || !display_resolution)
        return -EINVAL;
    const int err = scale_check_sizes(ref, dist);
    if (err)
        return err;

    const unsigned w = ref->w[0];
    const unsigned h = ref->h[0];
    const bool portrait = h > w;
    const unsigned short_side = portrait ? w : h;
    const unsigned long_side = portrait ? h : w;

    /* already at the display size: keep the source's dimensions exactly */
    if (short_side == display_resolution) {
        target->w = w;
        target->h = h;
        target->resample_filter = VMAF_RESAMPLE_DEFAULT;
        return 0;
    }

    /* long side at the source's aspect ratio, rounded to the nearest even */
    const unsigned long long scaled =
        (((unsigned long long)long_side * display_resolution) + short_side) / (2ULL * short_side) *
        2;
    /* wider than 4:1 (32:9 is ~3.6:1) is almost certainly a bad input, and
     * the scaled picture would be huge */
    if (scaled > 4ULL * display_resolution) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "scaling %ux%u to a display resolution of %u would give a "
                 "long side of %llu, over the limit of %u\n",
                 w, h, display_resolution, scaled, 4 * display_resolution);
        return -EINVAL;
    }

    target->w = portrait ? display_resolution : (unsigned)scaled;
    target->h = portrait ? (unsigned)scaled : display_resolution;
    target->resample_filter =
        short_side < display_resolution ? VMAF_RESAMPLE_BICUBIC : VMAF_RESAMPLE_LANCZOS;
    *needs_conversion = true;
    return 0;
}
