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

#ifndef FEATURE_COMPAT_BUILTIN_H_
#define FEATURE_COMPAT_BUILTIN_H_

/*
 * MSVC (but not clang-cl) lacks GCC's __builtin_clz / __builtin_clzll.
 *
 * DO NOT implement these with `__lzcnt` / `__lzcnt64`, which is what this
 * header used to do and what Netflix/vmaf#1422 proposes: MSVC emits the LZCNT
 * instruction (`F3 0F BD`) unconditionally, with no runtime feature gate. On
 * an x86-64 without ABM/LZCNT (Intel Core 2 through Ivy Bridge, AMD pre-
 * Barcelona) the `F3` prefix is IGNORED and the encoding retires as BSR,
 * returning the INDEX of the most-significant set bit instead of the
 * leading-zero COUNT. No fault, no diagnostic — just silently wrong VIF and
 * ADM fixed-point shifts:
 *
 *   integer_vif.h::log2_32   k = 16 - clz(temp)  -> off by 2048 LSBs, i.e.
 *                                                   a factor of two in the
 *                                                   VIF log2 fixed point
 *   integer_adm.c::get_best15_from32  k = 17 - clz(temp) -> shift by a
 *                                                   negative count (UB)
 *
 * Both of those live on the GENERIC scalar path (no SIMD dispatch gate), so
 * they run on every CPU an MSVC-built vmaf.exe lands on, while the CI runners
 * are all LZCNT-capable and never see it. Netflix/vmaf#1551 is upstream's own
 * retraction of the #1422 form.
 *
 * `_BitScanReverse` / `_BitScanReverse64` compile to BSR, which every x86-64
 * part has, so `31 - BSR` is correct everywhere and bit-identical to LZCNT on
 * hardware that has it. The zero input returns 32 / 64 (GCC's
 * `__builtin_clz(0)` is undefined; defining it here is strictly safer).
 *
 * The guard carries an explicit architecture allowlist. Per the MSVC intrinsics
 * reference, `_BitScanReverse` is available on x86, ARM, x64 AND ARM64, while
 * `_BitScanReverse64` is available on x64 and ARM64 only -- hence the two-step
 * fallback that reconstructs a 64-bit count from two 32-bit scans on the
 * remaining targets. `__lzcnt` by contrast really is x86-only, and is not used
 * here at all (see above).
 *
 * The allowlist is deliberate rather than a bare `defined(_MSC_VER)`, and it
 * must enumerate every architecture MSVC targets. This header supplies
 * `__builtin_clz` for integer_adm.c and integer_vif.h, which sit on the GENERIC
 * scalar path and are therefore compiled for every target: an MSVC
 * architecture that falls through the guard gets no definition at all and
 * fails to compile. That is exactly what happened to ARM64, which an earlier
 * `_M_X64 || _M_IX86` test excluded on the incorrect premise that
 * `_BitScanReverse` was x86-only. scripts/ci/check-msvc-clz-shim.sh pins the
 * ARM64 arm of the allowlist so it cannot be dropped again.
 *
 * The names stay as-is: the four call sites (integer_vif.h, integer_adm.c,
 * x86/adm_avx2.c, x86/adm_avx512.c) are upstream-verbatim and the rebase story
 * depends on them keeping the `__builtin_clz` spelling (ADR-0141 §2).
 */
/*
 * Leading-zero count derived from a most-significant-bit index — the shape
 * `_BitScanReverse` / `_BitScanReverse64` return.  Defined unconditionally
 * (outside the MSVC guard) so the host CI can unit-test the arithmetic that
 * distinguishes a correct shim from the BSR-vs-LZCNT confusion; see
 * core/test/test_compat_clz.c.
 */
static inline int vmaf_compat_clz32_from_msb(int found, unsigned msb_index)
{
    return found ? (int)(31u - msb_index) : 32;
}

static inline int vmaf_compat_clz64_from_msb(int found, unsigned msb_index)
{
    return found ? (int)(63u - msb_index) : 64;
}

#if defined(_MSC_VER) && !defined(__clang__) &&                                                    \
    (defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64) || defined(_M_ARM64EC) ||            \
     defined(_M_ARM))
#include <intrin.h>

static inline int __builtin_clz(unsigned x)
{
    unsigned long idx = 0;
    const int found = _BitScanReverse(&idx, (unsigned long)x) != 0;
    return vmaf_compat_clz32_from_msb(found, (unsigned)idx);
}

static inline int __builtin_clzll(unsigned long long x)
{
    unsigned long idx = 0;
#if defined(_M_X64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    const int found = _BitScanReverse64(&idx, x) != 0;
    return vmaf_compat_clz64_from_msb(found, (unsigned)idx);
#else
    if (_BitScanReverse(&idx, (unsigned long)(x >> 32)) != 0)
        return vmaf_compat_clz32_from_msb(1, (unsigned)idx);
    if (_BitScanReverse(&idx, (unsigned long)x) != 0)
        return vmaf_compat_clz64_from_msb(1, (unsigned)idx);
    return 64;
#endif
}
#endif

#endif /* FEATURE_COMPAT_BUILTIN_H_ */
