/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#ifndef ADM_CSF_TOOLS_H_
#define ADM_CSF_TOOLS_H_

#include <math.h>
#include "common/macros.h"

/*
 * CSF used in the DLM paper:
 * Image Quality Assessment by Separately Evaluating Detail Losses and Additive Impairments
 * Songnan Li, Fan Zhang, Lin Ma, King Ngi Ngan, IEEE Transactions on Multimedia 13(5):935-949
 */
FORCE_INLINE float adm_native_csf(int lambda, double adm_norm_view_dist, int adm_ref_display_height,
                                  int theta)
{
    /* This is the display visual resolution (DVR), in pixels/degree of visual angle. It should be ~56.55. */
    float r = (float)(adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0);
    /* This is the nominal spatial frequency for each DWT level; first level (level = 0) is half of the DVR. */
    float spatial_frequency = (float)(r / pow(2, lambda + 1));

    /*
     * Oblique effect: the HVS is more sensitive to the horizontal and vertical
     * channels than the diagonal channels.
     */
    if (theta == 45) {
        spatial_frequency = (float)(spatial_frequency / (0.7));
    }

    return (float)((0.31 + 0.69 * spatial_frequency) * exp(-0.29 * spatial_frequency));
}

#endif /* ADM_CSF_TOOLS_H_ */
