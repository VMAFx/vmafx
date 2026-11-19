/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#ifndef VMAF_BUILT_IN_MODEL_DATA_H_
#define VMAF_BUILT_IN_MODEL_DATA_H_

#include "config.h"

/* Match xxd -i object types exactly; keep read-only access at use sites. */
#if VMAF_BUILT_IN_MODELS
#if VMAF_FLOAT_FEATURES
extern unsigned char src_vmaf_float_v0_6_1neg_json[];
extern unsigned int src_vmaf_float_v0_6_1neg_json_len;
extern unsigned char src_vmaf_float_v0_6_1_json[];
extern unsigned int src_vmaf_float_v0_6_1_json_len;
extern unsigned char src_vmaf_float_b_v0_6_3_json[];
extern unsigned int src_vmaf_float_b_v0_6_3_json_len;
extern unsigned char src_vmaf_float_4k_v0_6_1_json[];
extern unsigned int src_vmaf_float_4k_v0_6_1_json_len;
#endif
extern unsigned char src_vmaf_v0_6_1_json[];
extern unsigned int src_vmaf_v0_6_1_json_len;
extern unsigned char src_vmaf_b_v0_6_3_json[];
extern unsigned int src_vmaf_b_v0_6_3_json_len;
extern unsigned char src_vmaf_v0_6_1neg_json[];
extern unsigned int src_vmaf_v0_6_1neg_json_len;
extern unsigned char src_vmaf_v0_6_1still_json[];
extern unsigned int src_vmaf_v0_6_1still_json_len;
extern unsigned char src_vmaf_4k_v0_6_1_json[];
extern unsigned int src_vmaf_4k_v0_6_1_json_len;
extern unsigned char src_vmaf_4k_v0_6_1neg_json[];
extern unsigned int src_vmaf_4k_v0_6_1neg_json_len;
/* VMAF v1.0.16 SDR models (ported from Netflix upstream 4718b4f5f). */
extern unsigned char src_vmaf_v1_0_16_3d0h_json[];
extern unsigned int src_vmaf_v1_0_16_3d0h_json_len;
extern unsigned char src_vmaf_v1_0_16_3d0h_2160_json[];
extern unsigned int src_vmaf_v1_0_16_3d0h_2160_json_len;
extern unsigned char src_vmaf_v1_0_16_5d0h_json[];
extern unsigned int src_vmaf_v1_0_16_5d0h_json_len;
extern unsigned char src_vmaf_v1_0_16_1d5h_2160_json[];
extern unsigned int src_vmaf_v1_0_16_1d5h_2160_json_len;
extern unsigned char src_vmaf_v1_0_16_hfr_3d0h_json[];
extern unsigned int src_vmaf_v1_0_16_hfr_3d0h_json_len;
extern unsigned char src_vmaf_v1_0_16_hfr_3d0h_2160_json[];
extern unsigned int src_vmaf_v1_0_16_hfr_3d0h_2160_json_len;
extern unsigned char src_vmaf_v1_0_16_hfr_5d0h_json[];
extern unsigned int src_vmaf_v1_0_16_hfr_5d0h_json_len;
extern unsigned char src_vmaf_v1_0_16_hfr_1d5h_2160_json[];
extern unsigned int src_vmaf_v1_0_16_hfr_1d5h_2160_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_3d0h_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_3d0h_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_3d0h_2160_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_3d0h_2160_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_5d0h_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_5d0h_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_1d5h_2160_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_1d5h_2160_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_hfr_3d0h_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_hfr_3d0h_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_hfr_3d0h_2160_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_hfr_3d0h_2160_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_hfr_5d0h_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_hfr_5d0h_json_len;
extern unsigned char src_hdr_vmaf_v1_1_8_hfr_1d5h_2160_json[];
extern unsigned int src_hdr_vmaf_v1_1_8_hfr_1d5h_2160_json_len;
#endif

#endif /* VMAF_BUILT_IN_MODEL_DATA_H_ */
