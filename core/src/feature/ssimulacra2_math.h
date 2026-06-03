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

#ifndef SSIMULACRA2_MATH_H_
#define SSIMULACRA2_MATH_H_

#include <stdint.h>

#include "feature/ssimulacra2_eotf_lut.h"

/*
 * Deterministic replacements for libm `cbrtf` and the sRGB EOTF
 * (`powf((x + 0.055)/1.055, 2.4)`) used by the SSIMULACRA 2
 * extractor and its SIMD ports.
 *
 * **Why**: libm `cbrtf` / `powf` implementations differ by ~1 ulp
 * between glibc, musl, and macOS libSystem — this compounded to a
 * ~2e-4 per-frame drift in the pooled ssimulacra2 score across
 * test hosts. These replacements are **identical across hosts**
 * (no libc dependency) and give the fork byte-for-byte
 * reproducibility for the ssimulacra2 snapshot gate (ADR-0164).
 *
 * Accuracy vs libm: ~7e-7 on cbrtf (Newton–Raphson, 2 iterations);
 * ~2e-7 on sRGB EOTF (1024-entry LUT + linear interpolation;
 * values pre-computed and committed as hex-float bit patterns).
 *
 * The fork's SIMD ports (AVX2 / AVX-512 / NEON) use the same
 * helpers, so scalar-vs-SIMD bit-exactness (ADR-0161/0162/0163
 * invariant) holds by construction.
 *
 * Accuracy vs the libjxl `tools/ssimulacra2` reference: ~1e-6 per
 * frame. ADR-0130 never committed to libjxl bit-exactness in CI,
 * so this is within scope.
 */

/* Deterministic cube root of a non-negative float.
 *
 * Bit-trick initial estimate via `(i / 3) + magic_constant` followed
 * by 2 Newton–Raphson iterations. Accuracy: < 1 ulp on inputs in
 * [1e-30, 1e+30]. Negative inputs return 0 (matches the scalar
 * reference's `if (l < 0) l = 0;` clamp in `linear_rgb_to_xyb`). */
static inline float vmaf_ss2_cbrtf(float x)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
    union {
        float f;
        uint32_t i;
    } u;
    u.f = x;
    /* Division by 3 of the exponent plus a bias that nudges the
     * fixed-point representation toward the cube root. Magic
     * constant is the classic one from Kahan / Schraudolph. */
    u.i = u.i / 3u + 0x2a5137a0u;
    float y = u.f;
    /* Two Newton iterations of f(y) = y^3 - x, y <- (2y + x/y^2) / 3. */
    y = (2.0f * y + x / (y * y)) * (1.0f / 3.0f);
    y = (2.0f * y + x / (y * y)) * (1.0f / 3.0f);
    return y;
}

/* Deterministic sRGB EOTF via precomputed LUT.
 *
 * The LUT (1024 entries spanning x in [0, 1]) is committed as
 * hardcoded float bit patterns in `ssimulacra2_eotf_lut.h` — the
 * generator script `scripts/gen_ssimulacra2_eotf_lut.py` runs
 * offline. Runtime is pure float arithmetic (index, frac, 2
 * loads, 1 lerp) with no libc dependency.
 *
 * Piecewise structure matches the scalar reference: for x <=
 * 0.04045 return x / 12.92; for x > 1.0 return 1.0 (clamp already
 * happens upstream in the caller, but guard for safety). The LUT
 * already folds in the piecewise branch (for index i <= 41 ≈
 * 0.04045 * 1023 the LUT value is i/1023/12.92), so the runtime
 * path needs only the one load + lerp. */
static inline float vmaf_ss2_srgb_eotf(float x)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return 1.0f;
    }
    const float idx = x * (float)(SS2_EOTF_LUT_SIZE - 1);
    const int i = (int)idx;
    const float frac = idx - (float)i;
    const float a = vmaf_ss2_eotf_lut[i];
    const float b = vmaf_ss2_eotf_lut[i + 1];
    return a + (b - a) * frac;
}

#endif /* SSIMULACRA2_MATH_H_ */
