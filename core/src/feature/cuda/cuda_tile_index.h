/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Index guard for the shared-memory tile loads of the CUDA feature kernels.
 *
 *  A tiled kernel loads a fixed tile per block: its own outputs plus the
 *  filter halo, for every thread of the block, including the padding threads
 *  past the last row or column of the plane. Each kernel reflects
 *  out-of-plane indices once, with the reflection its CPU reference uses. One
 *  reflection is enough for every sample a valid output consumes, but not for
 *  the padding rows and columns: on a plane smaller than the tile a single
 *  reflection of those lands below zero or past the end, and the load reads
 *  outside the allocation. The value is never consumed, but the read can
 *  fault the context. The SYCL twins hit exactly this on small frames
 *  (T-SYCL-TILE-HALO-OOB-READ-2026-09-29, sycl_tile_index.h).
 *
 *  Tile loaders pass the reflected index through vmaf_cuda_tile_index(). It is
 *  the identity for every index already inside the plane, so it cannot change
 *  a consumed sample or a score; it only keeps the unconsumed loads in bounds.
 *
 *  Plain C as well as CUDA C++, so device-free host tests can include it and
 *  check the kernels' index arithmetic (ADR-1374).
 */

#ifndef VMAF_SRC_FEATURE_CUDA_CUDA_TILE_INDEX_H_
#define VMAF_SRC_FEATURE_CUDA_CUDA_TILE_INDEX_H_

#if defined(__CUDACC__)
#define VMAF_CUDA_HOST_DEVICE __host__ __device__ __forceinline__
#else
#define VMAF_CUDA_HOST_DEVICE static inline
#endif

/* Clamp an already-reflected tile index into [0, extent - 1]. */
VMAF_CUDA_HOST_DEVICE int vmaf_cuda_tile_index(int reflected, int extent)
{
    if (reflected < 0) {
        return 0;
    }
    return (reflected >= extent) ? extent - 1 : reflected;
}

/* Reflect-101, the CPU motion's mirror(): -1 -> 1, extent -> extent - 2.
 * One reflection, like the CPU; callers clamp the result with
 * vmaf_cuda_tile_index() for the unconsumed padding samples.
 *
 * Precondition for the consumed samples: extent >= radius + 1 (3 for the
 * 5-tap motion filter). One reflection of an index within `radius` of the
 * plane then lands inside it, as the CPU's mirror() does. Below that the
 * CPU's mirror() itself would read outside the plane, so the motion twins'
 * init() refuses such frames, like the CPU extractors, and the clamp here is
 * never what makes a consumed sample valid. */
VMAF_CUDA_HOST_DEVICE int vmaf_cuda_reflect_101(int idx, int extent)
{
    if (idx < 0) {
        return -idx;
    }
    return (idx >= extent) ? (2 * extent) - idx - 2 : idx;
}

#endif /* VMAF_SRC_FEATURE_CUDA_CUDA_TILE_INDEX_H_ */
