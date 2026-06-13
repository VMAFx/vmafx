/**
 *
 *  Copyright 2016-2022 Netflix, Inc.
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

#include "config.h"

#include "arm/cpu.h"

#if defined(__linux__) && defined(ARCH_AARCH64)
#include <sys/auxv.h>
#ifndef HWCAP2_SVE2
/* Older glibc (<2.33) may lack the HWCAP2_SVE2 macro. The Linux ABI
 * value is stable across kernel versions — bit 1 in AT_HWCAP2 on
 * aarch64 (per linux/arch/arm64/include/uapi/asm/hwcap.h). Define
 * locally so the build does not depend on a recent glibc header. */
#define HWCAP2_SVE2 (1UL << 1)
#endif
#endif

unsigned vmaf_get_cpu_flags_arm(void)
{
    unsigned flags = 0;

#ifdef ARCH_AARCH64
    flags |= VMAF_ARM_CPU_FLAG_NEON;
#if defined(__linux__)
    /* T7-38: probe SVE2 at runtime. Failure to detect simply leaves
     * the bit clear and the dispatcher falls back to NEON. */
    const unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if (hwcap2 & HWCAP2_SVE2) {
        flags |= VMAF_ARM_CPU_FLAG_SVE2;
    }
#endif
#endif

    return flags;
}
