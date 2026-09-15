/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef VMAF_FEX_CTX_VECTOR_INTERNAL_H
#define VMAF_FEX_CTX_VECTOR_INTERNAL_H

#include <climits>
#include <cstdint>

struct VmafFeatureExtractorContext;

// Both the unsigned count and the allocation's byte size must represent growth.
// Returning zero rejects unrepresentable doubling before any state is changed.
constexpr unsigned vmaf_next_fex_capacity(unsigned capacity) noexcept
{
    if (capacity == 0 || capacity > UINT_MAX / 2u ||
        capacity > (SIZE_MAX / sizeof(VmafFeatureExtractorContext *)) / 2u)
        return 0;
    return capacity * 2u;
}

#endif // VMAF_FEX_CTX_VECTOR_INTERNAL_H
