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

#ifndef VMAF_SRC_CONVERSION_POLICY_H_
#define VMAF_SRC_CONVERSION_POLICY_H_

#include <stdbool.h>

#include "libvmaf/picture.h"

/**
 * Decide whether `ref` and `dist` need conversion before feature
 * extraction, given the target the model(s) in use declare.
 *
 * `model_target` is the model's declared conversion target, or NULL when
 * the model declares none. A model without a target is not
 * colorspace-aware, so this is a pass-through: `*needs_conversion` is set
 * to false and `target` is left untouched, regardless of how `ref` and
 * `dist` are tagged.
 *
 * Fork adaptation (ADR-1822, HISS-14): `VmafPicture` carries no colour, so the
 * source colour of each picture is an argument (`ref_color`, `dist_color`)
 * where upstream reads `pic->color`. The colour is the one the caller gave
 * `vmaf_set_input_colorimetry()`.
 *
 * When the model declares a target, `ref` and `dist` must each have all of
 * `range`, `primaries`, `trc` and `matrix` specified. Guessing the source
 * colorimetry would yield a plausible but wrong score, so anything less
 * (including a caller that never set the input colorimetry, which is
 * indistinguishable from `*_UNKNOWN`) returns -EINVAL and logs which
 * attributes are missing.
 *
 * Otherwise `target` is filled from `*model_target`: `color` always, and
 * `pix_fmt` / `bpc` when the model pins them (UNKNOWN / 0 otherwise, meaning
 * "keep each picture's own"). `*needs_conversion` is true unless both `ref`
 * and `dist` already match the target. `ref` and `dist` are allowed to
 * differ from each other: each is converted to the same target. The size
 * (`target->w`/`h`/`resample_filter`) is left to
 * vmaf_conversion_policy_scale(); w/h 0 keep each picture's own.
 */
int vmaf_conversion_policy_target(const VmafPicture *ref, const VmafColor *ref_color,
                                  const VmafPicture *dist, const VmafColor *dist_color,
                                  const VmafPictureConvertTarget *model_target,
                                  bool *needs_conversion, VmafPictureConvertTarget *target);

/** True when all of `a`'s color attributes equal `b`'s. */
bool vmaf_conversion_policy_color_equal(const VmafColor *a, const VmafColor *b);

/**
 * True when `pic` already matches `target`: same color, and the same pixel
 * format, bit depth and size wherever `target` pins them.
 */
bool vmaf_conversion_policy_picture_matches(const VmafPicture *pic, const VmafColor *pic_color,
                                            const VmafPictureConvertTarget *target);

/** True when two targets are the same in color, pixel format and bit depth. */
bool vmaf_conversion_policy_target_equal(const VmafPictureConvertTarget *a,
                                         const VmafPictureConvertTarget *b);

/**
 * Set the size of `target` so both pictures are scaled to a model's display
 * resolution (Netflix/vmaf ca2a2a111d). `display_resolution` is the short
 * side: 1080 means 1920x1080 or 1080x1920 by the source's orientation.
 *
 * `ref` and `dist` must be the same size; -EINVAL (logged) otherwise. A source
 * whose short side already equals `display_resolution` keeps its dimensions
 * exactly and leaves `*needs_conversion` as it was. Otherwise the long side
 * keeps the source's aspect ratio, rounded to the nearest even number,
 * upscaling is bicubic and downscaling lanczos, and `*needs_conversion` is set
 * to true. A long side over 4 * `display_resolution` is -EINVAL (logged).
 * NULL arguments and `display_resolution` 0 are -EINVAL.
 */
int vmaf_conversion_policy_scale(const VmafPicture *ref, const VmafPicture *dist,
                                 unsigned display_resolution, bool *needs_conversion,
                                 VmafPictureConvertTarget *target);

#endif /* VMAF_SRC_CONVERSION_POLICY_H_ */
