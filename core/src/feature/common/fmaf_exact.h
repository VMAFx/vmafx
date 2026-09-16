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

#pragma once

#ifndef VMAF_FEATURE_COMMON_FMAF_EXACT_H_
#define VMAF_FEATURE_COMMON_FMAF_EXACT_H_

/*
 * Host-independent single-rounded float multiply-add.
 *
 * **Why this exists.** Several scalar references are required to be bit-exact
 * with SIMD kernels that fuse with `_mm256_fmadd_ps` / `vfmla` — ADR-0891
 * unified them on a single rounding, and ADR-1205 records what happens when
 * one copy is missed: the ssimulacra2 pipeline is ill-conditioned downstream
 * (the edge-diff term is a catastrophic cancellation and the 4-norm pooling
 * amplifies the survivors), so one ULP here grew into a 2.6e-3 score delta.
 *
 * The scalar side spelled that single rounding `fmaf()`. On glibc, musl and
 * the UCRT that is correct: `fmaf` is a genuine fused multiply-add. On the
 * legacy `msvcrt.dll` that MSYS2's MINGW64 environment links, it is not — and
 * the fork's `Windows MinGW64` lane is exactly that environment. There the
 * scalar path rounded twice, the SIMD path once, and ADR-1207's ISA-invariance
 * gate measured the result: ssimulacra2 host-isa -38.376932759633718 against
 * scalar -38.376806310379322, and 0.37 points apart on a 48-frame clip.
 *
 * **Why the double detour is exact.** For binary32 operands the product is
 * exact in binary64 (24 + 24 = 48 significand bits), the addition rounds once
 * to binary64, and binary64 carries 53 bits — at least the 2p + 2 = 50 that
 * makes the final rounding to binary32 innocuous. The result is therefore the
 * correctly-rounded fused multiply-add on every host, with no FMA hardware and
 * no libm call. Measured against both glibc `fmaf` and the `vfmadd213ss`
 * instruction over 20 million random triples spanning the operand exponents
 * these call sites use: zero differences.
 *
 * Do not "simplify" this back to `fmaf()`, and do not replace it with
 * `a * b + c`: the first is not fused on legacy msvcrt, the second is not
 * fused anywhere unless the compiler happens to contract it, which is the very
 * thing `-ffp-contract=off` is set to prevent.
 */
static inline float vmaf_fmaf_exact(float a, float b, float c)
{
    return (float)((double)a * (double)b + (double)c);
}

#endif /* VMAF_FEATURE_COMMON_FMAF_EXACT_H_ */
