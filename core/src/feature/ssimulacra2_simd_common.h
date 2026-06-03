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

#ifndef SSIMULACRA2_SIMD_COMMON_H_
#define SSIMULACRA2_SIMD_COMMON_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Shared plane descriptor used by the SSIMULACRA 2 SIMD
 * `picture_to_linear_rgb_*` entry points (ADR-0163). Decouples the
 * per-ISA TUs from the `VmafPicture` type — the dispatch wrapper in
 * ssimulacra2.c unpacks `VmafPicture` fields into this struct
 * before calling the SIMD kernel.
 *
 * `data` points at the first pixel of the plane; stride is in bytes.
 * `w` / `h` are the plane's native dimensions (may be subsampled
 * relative to the luma plane for chroma).
 */
typedef struct {
    const void *data;
    ptrdiff_t stride;
    unsigned w;
    unsigned h;
} simd_plane_t;

#endif /* SSIMULACRA2_SIMD_COMMON_H_ */
