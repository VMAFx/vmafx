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
           (!target->bpc || pic->bpc == target->bpc);
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
