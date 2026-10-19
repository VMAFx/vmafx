/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The largest plane a Metal twin may index with a 32-bit uint
 * (T-METAL-UINT-PLANE-INDEX-2026-10-05; core/src/feature/metal/metal_plane_index.h).
 *
 * The Metal kernels of float_vif, integer SSIM, float_ssim and float_ms_ssim
 * index five moment planes of N samples as k * N + at in uint. The check
 * accepts N up to 858,993,459 (largest index 2^32 - 1) and refuses one more;
 * 16K at the default options is accepted, 16K with float_vif's prescale 2.6
 * and the 32768 x 32768 picture cap are refused. Device-free and built on every
 * platform: the header is plain C. The Metal parity tests hold each twin's
 * init() to the same answer on an Apple device.
 */

#include <errno.h>
#include <stdint.h>

#include "test.h"

#include "feature/metal/metal_plane_index.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define LIMIT (UINT64_C(858993459))

static char *test_the_boundary_is_the_last_uint_index(void)
{
    mu_assert("five planes of the limit fill the uint range exactly",
              LIMIT * VMAF_MTL_MOMENT_PLANES == UINT64_C(4294967295));
    mu_assert("the limit is accepted",
              vmaf_mtl_plane_index_check("test", LIMIT, VMAF_MTL_MOMENT_PLANES) == 0);
    mu_assert("one sample more is refused",
              vmaf_mtl_plane_index_check("test", LIMIT + 1u, VMAF_MTL_MOMENT_PLANES) == -EINVAL);
    mu_assert("one plane of 2^32 samples is accepted",
              vmaf_mtl_plane_index_check("test", UINT64_C(4294967296), 1u) == 0);
    mu_assert("no planes is accepted", vmaf_mtl_plane_index_check("test", UINT64_MAX, 0u) == 0);
    return NULL;
}

static char *test_the_sizes_of_the_audit(void)
{
    /* 16K at the default options, and the float_ssim / float_ms_ssim planes. */
    mu_assert("16K refused", vmaf_mtl_plane_index_check("test", UINT64_C(15360) * 8640u,
                                                        VMAF_MTL_MOMENT_PLANES) == 0);
    mu_assert("16K (w - 10) refused", vmaf_mtl_plane_index_check("test", UINT64_C(15350) * 8640u,
                                                                 VMAF_MTL_MOMENT_PLANES) == 0);
    /* float_vif at 16K, vif_prescale 2.6: 39936 x 22464. */
    mu_assert("16K at prescale 2.6 accepted",
              vmaf_mtl_plane_index_check("test", UINT64_C(39936) * 22464u,
                                         VMAF_MTL_MOMENT_PLANES) == -EINVAL);
    /* The picture cap, VMAF_PIC_DIM_MAX x VMAF_PIC_DIM_MAX. */
    mu_assert("the cap accepted", vmaf_mtl_plane_index_check("test", UINT64_C(32768) * 32768u,
                                                             VMAF_MTL_MOMENT_PLANES) == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_the_boundary_is_the_last_uint_index);
    mu_run_test(test_the_sizes_of_the_audit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
