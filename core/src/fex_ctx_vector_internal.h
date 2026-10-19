/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_FEX_CTX_VECTOR_INTERNAL_H
#define VMAF_FEX_CTX_VECTOR_INTERNAL_H

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>

struct VmafFeatureExtractorContext;

// Both the unsigned count and the allocation's byte size must represent growth.
// Returning zero rejects unrepresentable doubling before any state is changed.
// The smaller of the two limits is one constant, so the test is one comparison
// on every target; the byte-size limit is the smaller one only where size_t is
// 32 bits wide.
constexpr unsigned vmaf_next_fex_capacity(unsigned capacity) noexcept
{
    constexpr std::uintmax_t count_limit = UINT_MAX / 2u;
    constexpr std::uintmax_t bytes_limit = (SIZE_MAX / sizeof(VmafFeatureExtractorContext *)) / 2u;
    // (std::min)(...): <windows.h> defines min as a function-like macro and is
    // read before this header on Windows; the parentheses keep the name from
    // being expanded (MSVC C2589 otherwise).
    constexpr auto limit = static_cast<unsigned>((std::min)(count_limit, bytes_limit));
    if (capacity == 0 || capacity > limit)
        return 0;
    return capacity * 2u;
}

#endif // VMAF_FEX_CTX_VECTOR_INTERNAL_H
