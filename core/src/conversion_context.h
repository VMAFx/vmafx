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

#ifndef VMAF_SRC_CONVERSION_CONTEXT_H_
#define VMAF_SRC_CONVERSION_CONTEXT_H_

#include <stdbool.h>

#include "libvmaf/picture.h"
#include "model.h"

/**
 * Conversion state of one VmafContext. Fork-local carrier for the glue
 * Netflix/vmaf a6c0ba6d5 keeps inside libvmaf.c: the target the registered
 * models declare, the zimg contexts converting each input, and the colorimetry
 * of each input. Upstream reads the latter from `VmafPicture::color`; the fork
 * keeps `VmafPicture` unchanged (ADR-1822, HISS-14) and takes it from
 * vmaf_set_input_colorimetry() (ADR-2093).
 */
typedef struct VmafConversionState {
    VmafPictureConvertContext *ref;
    VmafPictureConvertContext *dist;
    bool have_target;
    VmafPictureConvertTarget target;
    unsigned n_models_with_target;
    unsigned n_models_without_target;
    VmafColor ref_color;
    VmafColor dist_color;
} VmafConversionState;

/**
 * Register the conversion target of `model`. All models of a run must declare
 * the same target, or none may; -EINVAL (logged) otherwise.
 */
int vmaf_conversion_state_register_model(VmafConversionState *state, const VmafModel *model);

/**
 * Set the colorimetry of the reference and distorted inputs; NULL leaves that
 * input unspecified. -EBUSY once a picture has been converted (the zimg
 * context is built from the first picture and its colour).
 */
int vmaf_conversion_state_set_input_color(VmafConversionState *state, const VmafColor *ref,
                                          const VmafColor *dist);

/**
 * Offer the colorimetry of the next pair (ADR-2094, the VMAFx submit): 0 when
 * it equals the colorimetry in use, else as
 * vmaf_conversion_state_set_input_color().
 */
int vmaf_conversion_state_offer_input_color(VmafConversionState *state, const VmafColor *ref,
                                            const VmafColor *dist);

/**
 * Convert `ref` and `dist` to the registered target. Pass-through when no
 * model declares one; -EINVAL when one does and an input's colorimetry is not
 * fully specified. On success a converted picture replaces its original, which
 * is released; on failure both originals are left untouched, for the caller to
 * release.
 */
int vmaf_conversion_state_convert(VmafConversionState *state, VmafPicture *ref, VmafPicture *dist);

/**
 * 0 when no registered model declares a conversion target; otherwise -ENOTSUP
 * after logging that `entry_point` hands the extractors device pictures that no
 * conversion reaches. A zero-copy entry point calls it so a model with a
 * target never scores unconverted pictures.
 */
int vmaf_conversion_state_refuse_zero_copy(const VmafConversionState *state,
                                           const char *entry_point);

/** Release the zimg contexts. */
void vmaf_conversion_state_close(VmafConversionState *state);

#endif /* VMAF_SRC_CONVERSION_CONTEXT_H_ */
