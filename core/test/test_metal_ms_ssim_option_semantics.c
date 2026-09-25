/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Device-free execution of the option semantics used by
 * float_ms_ssim_metal.mm. This runs on every host; the Apple-Silicon parity
 * test remains the end-to-end device gate. ADR-1334. */

#include <math.h>
#include <stdbool.h>

#include "test.h"

#include "feature/metal/float_ms_ssim_option_semantics.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; ADR-1138. */

static char *test_max_db_ceiling_semantics(void)
{
    const double unclipped = vmaf_metal_ms_ssim_max_db(false, 8u, 512u, 384u);
    mu_assert("clip_db=false must preserve the intentional +Inf ceiling",
              isinf(unclipped) && unclipped > 0.0);

    const double clipped = vmaf_metal_ms_ssim_max_db(true, 8u, 512u, 384u);
    mu_assert("512x384 8-bit clip_db ceiling must be the worked 105 dB value", clipped == 105.0);
    return NULL;
}

static char *test_enable_chroma_plane_count(void)
{
    mu_assert("default path must stay luma-only",
              vmaf_metal_ms_ssim_active_planes(false, VMAF_PIX_FMT_YUV420P) == 1u);
    mu_assert("4:2:0 enable_chroma must activate three planes",
              vmaf_metal_ms_ssim_active_planes(true, VMAF_PIX_FMT_YUV420P) == 3u);
    mu_assert("4:0:0 must stay luma-only even when enable_chroma is requested",
              vmaf_metal_ms_ssim_active_planes(true, VMAF_PIX_FMT_YUV400P) == 1u);
    return NULL;
}

static char *test_subsampled_plane_geometry(void)
{
    unsigned width = 0u;
    unsigned height = 0u;
    vmaf_metal_ms_ssim_plane_dimensions(VMAF_PIX_FMT_YUV420P, 1u, 351u, 353u, &width, &height);
    mu_assert("4:2:0 chroma must use ceil-halved dimensions", width == 176u && height == 177u);

    vmaf_metal_ms_ssim_plane_dimensions(VMAF_PIX_FMT_YUV422P, 2u, 351u, 177u, &width, &height);
    mu_assert("4:2:2 chroma must halve width only", width == 176u && height == 177u);

    vmaf_metal_ms_ssim_plane_dimensions(VMAF_PIX_FMT_YUV444P, 2u, 176u, 177u, &width, &height);
    mu_assert("4:4:4 chroma must preserve both dimensions", width == 176u && height == 177u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_max_db_ceiling_semantics);
    mu_run_test(test_enable_chroma_plane_count);
    mu_run_test(test_subsampled_plane_geometry);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
