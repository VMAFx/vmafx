/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The device twins declared exact (scripts/ci/exact_twins.d/<cell>.<backend>,
 * ADR-1428) that the CUDA and HIP import tests compare, each as the CPU
 * extractor and options a VMAFx context on a device registers for it (the
 * aliases of scripts/ci/cross_backend_parity_gate.py FEATURE_ALIASES). The
 * CUDA (ADR-2023) and HIP (ADR-2092) lanes declare the same cells;
 * core/test/test_vmafx_import_cuda_cells_contract.py holds the table equal to
 * the .cuda and the .hip fragment files. The helpers (one context per cell,
 * the bit-for-bit comparison) are vmafx_exact_cells.h's, shared with the SYCL
 * lane's table (vmafx_sycl_cells.h).
 */

#ifndef VMAFX_DEVICE_CELLS_H
#define VMAFX_DEVICE_CELLS_H

#include "vmafx_exact_cells.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

static const VcCell vc_cells[] = {
    {"adm", "adm", NULL, 0u},
    {"cambi", "cambi", NULL, 0u},
    {"float_adm", "float_adm", NULL, 0u},
    {"float_moment", "float_moment", NULL, 0u},
    {"float_motion", "float_motion", NULL, 0u},
    {"float_ms_ssim", "float_ms_ssim", NULL, 0u},
    {"float_ms_ssim_chroma", "float_ms_ssim", "enable_chroma=true", 176u},
    {"float_ms_ssim_lcs", "float_ms_ssim", "enable_lcs=true", 0u},
    {"float_psnr", "float_psnr", NULL, 0u},
    {"float_ssim", "float_ssim", NULL, 0u},
    {"float_ssim_lcs", "float_ssim", "enable_lcs=true", 0u},
    {"float_vif", "float_vif", NULL, 0u},
    {"motion", "motion", NULL, 0u},
    {"motion_debug", "motion", "debug=true", 0u},
    {"motion_mffw", "motion", "motion_five_frame_window=true:motion_moving_average=true", 0u},
    {"motion_v2", "motion_v2", NULL, 0u},
    {"motion_v2_mffw", "motion_v2", "motion_five_frame_window=true:motion_moving_average=true", 0u},
    {"psnr", "psnr", NULL, 0u},
    {"psnr_hvs", "psnr_hvs", NULL, 0u},
    {"speed_chroma", "speed_chroma", NULL, 0u},
    {"speed_temporal", "speed_temporal", NULL, 0u},
    {"ssim", "ssim", NULL, 0u},
    {"ssimulacra2", "ssimulacra2", NULL, 0u},
    {"vif", "vif", NULL, 0u},
};

#define VC_N_CELLS (sizeof(vc_cells) / sizeof(vc_cells[0]))

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_DEVICE_CELLS_H */
