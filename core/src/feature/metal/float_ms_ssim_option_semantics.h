/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_FEATURE_METAL_FLOAT_MS_SSIM_OPTION_SEMANTICS_H
#define VMAF_FEATURE_METAL_FLOAT_MS_SSIM_OPTION_SEMANTICS_H

#include <math.h>
#include <stdbool.h>

#include "libvmaf/picture.h"

/* ADR-1334: device-free option semantics shared by the Metal host implementation
 * and its Linux/Windows regression test. Keep Metal framework types out of this file. */
static inline unsigned vmaf_metal_ms_ssim_active_planes(bool enable_chroma,
                                                        enum VmafPixelFormat pix_fmt)
{
    return enable_chroma && pix_fmt != VMAF_PIX_FMT_YUV400P ? 3u : 1u;
}

static inline void vmaf_metal_ms_ssim_plane_dimensions(enum VmafPixelFormat pix_fmt, unsigned plane,
                                                       unsigned width, unsigned height,
                                                       unsigned *plane_width,
                                                       unsigned *plane_height)
{
    const unsigned ss_hor =
        pix_fmt == VMAF_PIX_FMT_YUV420P || pix_fmt == VMAF_PIX_FMT_YUV422P ? 1u : 0u;
    const unsigned ss_ver = pix_fmt == VMAF_PIX_FMT_YUV420P ? 1u : 0u;
    *plane_width = plane == 0u ? width : (width + ss_hor) >> ss_hor;
    *plane_height = plane == 0u ? height : (height + ss_ver) >> ss_ver;
}

static inline void vmaf_metal_ms_ssim_min_luma_dimensions(enum VmafPixelFormat pix_fmt,
                                                          unsigned plane_minimum,
                                                          unsigned *minimum_width,
                                                          unsigned *minimum_height)
{
    const unsigned ss_hor =
        pix_fmt == VMAF_PIX_FMT_YUV420P || pix_fmt == VMAF_PIX_FMT_YUV422P ? 1u : 0u;
    const unsigned ss_ver = pix_fmt == VMAF_PIX_FMT_YUV420P ? 1u : 0u;
    *minimum_width = (plane_minimum << ss_hor) - ss_hor;
    *minimum_height = (plane_minimum << ss_ver) - ss_ver;
}

static inline double vmaf_metal_ms_ssim_max_db(bool clip_db, unsigned bpc, unsigned width,
                                               unsigned height)
{
    if (!clip_db)
        return INFINITY;
    const unsigned peak = (1u << bpc) - 1u;
    const double mse = 0.5 / ((double)width * (double)height);
    return ceil(10. * log10((double)peak * (double)peak / mse));
}

#endif /* VMAF_FEATURE_METAL_FLOAT_MS_SSIM_OPTION_SEMANTICS_H */
