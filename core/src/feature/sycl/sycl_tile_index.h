/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Index guard for the local-memory tile loads of the SYCL feature kernels.
 *
 *  A tiled kernel loads a fixed tile per work-group: its own outputs plus the
 *  filter halo, for every work-item of the group, including the padding
 *  work-items past the last row or column of the plane. Each TU reflects
 *  out-of-plane indices once, with the reflection its CPU reference uses. One
 *  reflection is enough for every sample a valid output consumes, but not for
 *  the padding rows and columns: on a plane smaller than the tile a single
 *  reflection of those can land below zero (the integer ADM vertical DWT tile
 *  reads row -1 of an 8-row plane) or past the end, and the load reads outside
 *  the USM allocation. The value is never consumed, but the read page-faults
 *  the device (UR_RESULT_ERROR_DEVICE_LOST) whenever the neighbouring page is
 *  unmapped, which depends on the driver's allocation layout
 *  (T-SYCL-TILE-HALO-OOB-READ-2026-09-29).
 *
 *  Tile loaders pass the reflected index through vmaf_sycl_tile_index(). It is
 *  the identity for every index already inside the plane, so it cannot change
 *  a consumed sample or a score; it only keeps the unconsumed loads in bounds.
 */

#ifndef VMAF_SRC_FEATURE_SYCL_SYCL_TILE_INDEX_H_
#define VMAF_SRC_FEATURE_SYCL_SYCL_TILE_INDEX_H_

/* Clamp an already-reflected tile index into [0, extent - 1]. */
inline int vmaf_sycl_tile_index(int reflected, int extent)
{
    if (reflected < 0) {
        return 0;
    }
    return (reflected >= extent) ? extent - 1 : reflected;
}

#endif /* VMAF_SRC_FEATURE_SYCL_SYCL_TILE_INDEX_H_ */
