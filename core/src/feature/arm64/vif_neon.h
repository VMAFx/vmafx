/**
 *
 *  Copyright 2026 Lusoris
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

#ifndef ARM64_VIF_H_
#define ARM64_VIF_H_

#include "feature/integer_vif.h"

void vif_subsample_rd_8_neon(const VifBuffer *buf, unsigned w, unsigned h);

void vif_subsample_rd_16_neon(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc);

void vif_statistic_8_neon(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h);

void vif_statistic_16_neon(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h,
                           int bpc, int scale);

#endif /* ARM64_VIF_H_ */
