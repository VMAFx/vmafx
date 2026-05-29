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

/**
 * resolution_dispatch.c — implementation of vmaf_cuda_workload_class().
 *
 * See resolution_dispatch.h for the full usage contract and ADR-0753 for the
 * policy rationale.
 */

#include "resolution_dispatch.h"

#include <assert.h>

enum WorkloadSize vmaf_cuda_workload_class(int w, int h)
{
    assert(w > 0);
    assert(h > 0);

    /* Use int64_t arithmetic to avoid integer overflow at e.g. 8K (7680×4320 =
     * 33177600, which fits in int32 but is closer to the edge than we want). */
    const long long pixels = (long long)w * (long long)h;

    if (pixels < (long long)VMAF_CUDA_PIXEL_THRESHOLD_MEDIUM) {
        return WS_SMALL;
    }
    if (pixels < (long long)VMAF_CUDA_PIXEL_THRESHOLD_LARGE) {
        return WS_MEDIUM;
    }
    return WS_LARGE;
}
