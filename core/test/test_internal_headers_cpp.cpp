/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Compile-only C++26 twin of test_internal_headers_c.c. It proves the same
 *  internal declarations retain their C ABI while using C++ standard headers
 *  and aliases instead of deprecated or ineffective C compatibility headers.
 */

#include <type_traits>

#include "feature/cambi_internal.h"
#include "feature/luminance_tools.h"
#include "libvmaf_priv.h"
#include "model.h"

static_assert(std::is_same_v<std::underlying_type_t<VmafPixelRange>, unsigned int>);
static_assert(std::is_same_v<std::underlying_type_t<VmafModelType>, unsigned int>);
static_assert(std::is_same_v<std::underlying_type_t<VmafModelNormalizationType>, unsigned int>);
static_assert(std::is_same_v<VmafCambiRangeUpdater, void (*)(std::uint16_t *, int, int)>);
static_assert(std::is_same_v<VmafContext, struct VmafContext>);
