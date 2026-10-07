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

#include "conversion_context.h"
#include "conversion_policy.h"
#include "log.h"
#include "picture_sample_range.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

static int convert_context_get(VmafPictureConvertContext **ctx, const VmafPicture *pic,
                               const VmafColor *pic_color,
                               const VmafPictureConvertTarget *model_target)
{
    if (*ctx)
        return 0;

    VmafPictureConvertTarget target = *model_target;
    if (!target.pix_fmt)
        target.pix_fmt = pic->pix_fmt;
    if (!target.bpc)
        target.bpc = pic->bpc;
    target.w = pic->w[0];
    target.h = pic->h[0];
    target.resample_filter = VMAF_RESAMPLE_DEFAULT;
    return vmaf_picture_convert_context_init_with_color(ctx, pic, pic_color, &target);
}

/* Convert `pic` to `target` into `out`. Leaves `out` untouched when `pic` already
 * matches it. */
static int convert_picture(VmafPictureConvertContext **ctx, const VmafPicture *pic,
                           const VmafColor *pic_color, const VmafPictureConvertTarget *target,
                           VmafPicture *out)
{
    if (vmaf_conversion_policy_picture_matches(pic, pic_color, target))
        return 0;
    /* zimg reads the planes on the host; a device picture cannot be converted. */
    if (!vmaf_picture_host_readable(pic)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "the model's conversion_target needs the pictures on the host, and one is in "
                 "device memory\n");
        return -ENOTSUP;
    }
    int err = convert_context_get(ctx, pic, pic_color, target);
    if (err)
        return err;
    return vmaf_picture_convert(*ctx, out, pic);
}

/* Release `pic`; a release failure does not hide `err`, the error being reported. */
static int release_picture(VmafPicture *pic, int err)
{
    const int release_err = vmaf_picture_unref(pic);
    return err ? err : release_err;
}

/* Replace each original that was converted by its converted picture. */
static int swap_converted(VmafPicture *ref, VmafPicture *ref_converted, VmafPicture *dist,
                          VmafPicture *dist_converted)
{
    int err = 0;
    if (ref_converted->priv) {
        err = release_picture(ref, err);
        *ref = *ref_converted;
    }
    if (dist_converted->priv) {
        err = release_picture(dist, err);
        *dist = *dist_converted;
    }
    return err;
}

int vmaf_conversion_state_convert(VmafConversionState *state, VmafPicture *ref, VmafPicture *dist)
{
    bool needs_conversion = false;
    VmafPictureConvertTarget target = {0};
    int err = vmaf_conversion_policy_target(ref, &state->ref_color, dist, &state->dist_color,
                                            state->have_target ? &state->target : NULL,
                                            &needs_conversion, &target);
    if (err)
        return err;
    if (!needs_conversion)
        return 0;

    VmafPicture ref_converted = {0};
    VmafPicture dist_converted = {0};
    err = convert_picture(&state->ref, ref, &state->ref_color, &target, &ref_converted);
    if (err)
        return err;
    err = convert_picture(&state->dist, dist, &state->dist_color, &target, &dist_converted);
    if (err) {
        if (ref_converted.priv)
            err = release_picture(&ref_converted, err);
        return err;
    }
    return swap_converted(ref, &ref_converted, dist, &dist_converted);
}

static int register_mismatch(const VmafModel *model)
{
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "model \"%s\" has a different conversion target than the models "
             "already in use; all models in a run must share one\n",
             model->name);
    return -EINVAL;
}

/* All models in a run share one set of pictures, so they must agree on the
 * conversion target: either none declare one, or all declare the same one. */
int vmaf_conversion_state_register_model(VmafConversionState *state, const VmafModel *model)
{
    if (!model->conversion_target.enabled) {
        if (state->n_models_with_target)
            return register_mismatch(model);
        state->n_models_without_target++;
        return 0;
    }

    if (state->n_models_without_target)
        return register_mismatch(model);
    const VmafPictureConvertTarget model_target = {
        .pix_fmt = model->conversion_target.pix_fmt,
        .bpc = model->conversion_target.bpc,
        .color = model->conversion_target.color,
    };
    if (state->have_target && !vmaf_conversion_policy_target_equal(&state->target, &model_target))
        return register_mismatch(model);
    state->have_target = true;
    state->target = model_target;
    state->n_models_with_target++;
    return 0;
}

int vmaf_conversion_state_set_input_color(VmafConversionState *state, const VmafColor *ref,
                                          const VmafColor *dist)
{
    if (state->ref || state->dist)
        return -EBUSY;
    const VmafColor unset = {0};
    state->ref_color = ref ? *ref : unset;
    state->dist_color = dist ? *dist : unset;
    return 0;
}

int vmaf_conversion_state_offer_input_color(VmafConversionState *state, const VmafColor *ref,
                                            const VmafColor *dist)
{
    if (vmaf_conversion_policy_color_equal(ref, &state->ref_color) &&
        vmaf_conversion_policy_color_equal(dist, &state->dist_color))
        return 0;
    return vmaf_conversion_state_set_input_color(state, ref, dist);
}

int vmaf_conversion_state_refuse_zero_copy(const VmafConversionState *state,
                                           const char *entry_point)
{
    if (!state->have_target)
        return 0;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "%s: a model declares a conversion_target, and this entry point scores device "
             "pictures that the conversion does not reach. Score the model from host pictures "
             "with vmaf_read_pictures().\n",
             entry_point);
    return -ENOTSUP;
}

void vmaf_conversion_state_close(VmafConversionState *state)
{
    if (state->ref && vmaf_picture_convert_context_close(state->ref))
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not close the reference conversion context\n");
    if (state->dist && vmaf_picture_convert_context_close(state->dist))
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not close the distorted conversion context\n");
    state->ref = NULL;
    state->dist = NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
