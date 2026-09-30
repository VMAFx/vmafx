/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host-side setup for the device-resident SpEED pipeline (ADR-1358): the
 *  init-time part of speed.c's speed_init() and filter_and_downscale() that
 *  both SYCL extractors share. Nothing here runs per frame.
 *
 *  The geometry, the filter taps and the scoring constants come from
 *  speed_internal_gpu_configure() (speed_internal.c), the routine the CUDA
 *  twins call too (ADR-1380), so every backend derives them from one
 *  implementation (HISS-19).
 */

#include "speed_sycl_pipeline.h"

int speed_sycl::configure(const SpeedInternalDimensions &dim, const SpeedInternalOptions &opt,
                          unsigned bpc, PipelineConfig &config)
{
    SpeedGpuConfig shared{};
    const int err = speed_internal_gpu_configure(&dim, &opt, bpc, &shared);
    if (err) {
        return err;
    }
    config.geometry = shared.geometry;
    config.filters = shared.filters;
    config.scoring = shared.scoring;
    return 0;
}
