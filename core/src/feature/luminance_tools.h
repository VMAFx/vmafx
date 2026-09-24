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

#ifndef VMAF_LUMINANCE_TOOLS_H_
#define VMAF_LUMINANCE_TOOLS_H_

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
using VmafEOTF = double (*)(double V);
#else
typedef double (*VmafEOTF)(double V);
#endif

/*
 * Limited pixel range means that only values between 16 and 235 will be used in 8 bits
 * (rescale the bounds appropriately for other bitdepths).
 * Full pixel range means that values from 0 to 2^bitdepth - 1 will be used.
 */
#ifdef __cplusplus
enum VmafPixelRange : unsigned int {
#else
enum VmafPixelRange {
#endif
    VMAF_PIXEL_RANGE_UNKNOWN = 0,
    VMAF_PIXEL_RANGE_LIMITED = 1,
    VMAF_PIXEL_RANGE_FULL = 2,
    VMAF_PIXEL_RANGE_ABI_UINT_MAX = UINT_MAX,
};

/*
 * Contains the necessary information to normalize a luma value down to [0, 1].
 */
struct VmafLumaRange {
    int foot;
    int head;
};

#ifndef __cplusplus
typedef struct VmafLumaRange VmafLumaRange;
#endif

/*
 * Constructor for the LumaRange struct.
 */
int vmaf_luminance_init_luma_range(VmafLumaRange *luma_range, int bitdepth,
                                   enum VmafPixelRange pix_range);

/*
 * Returns the EOTF corresponding to the string given.
 * eotf_str must be one of ['bt1886', 'pq']
 */
int vmaf_luminance_init_eotf(VmafEOTF *eotf, const char *eotf_str);

/*
 * Takes a normalized luma value in the [0, 1] range and returns a luminance value.
 */
double vmaf_luminance_bt1886_eotf(double V);

/*
 * Takes a normalized luma value in the [0, 1] range and returns a luminance value.
 */
double vmaf_luminance_pq_eotf(double V);

/*
 * Takes a luma value, normalizes it and applies the given VmafEOTF
 * to return a luminance value.
 */
double vmaf_luminance_get_luminance(int sample, VmafLumaRange luma_range, VmafEOTF eotf);

/* Narrow internal trampolines for unit-test access to file-local helpers. */
#ifdef __cplusplus
#define VMAF_NOEXCEPT noexcept
#else
#define VMAF_NOEXCEPT
#endif

int vmaf_luminance_test_range_foot_head(int bitdepth, int pix_range, int *foot,
                                        int *head) VMAF_NOEXCEPT;
double vmaf_luminance_test_normalize_range(int sample, VmafLumaRange range) VMAF_NOEXCEPT;

#undef VMAF_NOEXCEPT

#ifdef __cplusplus
}
#endif

#endif
