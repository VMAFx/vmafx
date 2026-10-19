/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef TEST_ADM_DECOUPLE_RECIP_CPU_H_
#define TEST_ADM_DECOUPLE_RECIP_CPU_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t adm_recip_cpu_table(int o);
int adm_recip_cpu_sample(int o, int t, int angle_flag, double gain);
/* The 32-bit pipeline's band (scales 1-3) and the angle flag, as the CPU forms them. */
int32_t adm_recip_cpu_sample_s123(int32_t o, int32_t t, int angle_flag, double gain);
int adm_recip_cpu_angle_flag(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq);

#ifdef __cplusplus
}
#endif

#endif /* TEST_ADM_DECOUPLE_RECIP_CPU_H_ */
