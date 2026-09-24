/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Compile-only C23 smoke for the internal headers introduced or extended by
 *  the CodeQL non-header-include cleanup. The sentinels in the internal enums
 *  deliberately preserve their historical unsigned-int ABI width.
 */

#include "feature/cambi_internal.h"
#include "feature/luminance_tools.h"
#include "libvmaf_priv.h"
#include "model.h"

_Static_assert(sizeof(enum VmafPixelRange) == sizeof(unsigned int),
               "VmafPixelRange must retain its unsigned-int ABI width");
_Static_assert(sizeof(enum VmafModelType) == sizeof(unsigned int),
               "VmafModelType must retain its unsigned-int ABI width");
_Static_assert(sizeof(enum VmafModelNormalizationType) == sizeof(unsigned int),
               "VmafModelNormalizationType must retain its unsigned-int ABI width");
_Static_assert(sizeof(VmafCambiHostBuffers) == sizeof(struct VmafCambiHostBuffers),
               "the C CAMBI host-buffer typedef must name its struct");
_Static_assert(sizeof(VmafLumaRange) == sizeof(struct VmafLumaRange),
               "the C luminance-range typedef must name its struct");
