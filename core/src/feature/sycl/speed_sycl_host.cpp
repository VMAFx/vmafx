/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host-side setup for the device-resident SpEED pipeline (ADR-1358): the
 *  init-time part of speed.c's speed_init() and filter_and_downscale() that
 *  both SYCL extractors share. Nothing here runs per frame.
 */

#include "speed_sycl_pipeline.h"

#include <cerrno>
#include <cmath>
#include <cstring>

#include "log.h"
#include "feature/speed_constants.h"
#include "vif_tools.h"

using speed_sycl::Filters;
using speed_sycl::Geometry;
using speed_sycl::kElements;
using speed_sycl::PipelineConfig;
using speed_sycl::Scoring;

namespace
{

/* picture_copy() takes its 16-bit path for exactly these depths and reads
 * every other depth as 8-bit samples. */
uint32_t sample_bytes(unsigned bpc)
{
    return (bpc == 10u || bpc == 12u || bpc == 16u) ? 2u : 1u;
}

float sample_scale(unsigned bpc)
{
    if (bpc == 10u) {
        return 4.0f;
    }
    if (bpc == 12u) {
        return 16.0f;
    }
    if (bpc == 16u) {
        return 256.0f;
    }
    return 1.0f;
}

void fill_geometry(const SpeedInternalDimensions &dim, const SpeedInternalOptions &opt,
                   unsigned bpc, int32_t method, Geometry &g)
{
    g.src_w = static_cast<uint32_t>(dim.original_width);
    g.src_h = static_cast<uint32_t>(dim.original_height);
    g.scaled_w = static_cast<uint32_t>(dim.scaled_width);
    g.scaled_h = static_cast<uint32_t>(dim.scaled_height);
    g.down_w = g.scaled_w >> SPEED_INTERNAL_NUM_SCALES;
    g.down_h = g.scaled_h >> SPEED_INTERNAL_NUM_SCALES;
    g.trunc_w = static_cast<uint32_t>(dim.truncated_width);
    g.trunc_h = static_cast<uint32_t>(dim.truncated_height);
    g.blocks_h = static_cast<uint32_t>(dim.num_blocks_horizontal);
    g.blocks = static_cast<uint32_t>(dim.num_blocks);
    g.sub_w = static_cast<uint32_t>(dim.submatrix_width);
    g.sub_h = static_cast<uint32_t>(dim.submatrix_height);
    g.bytes_per_sample = sample_bytes(bpc);
    g.sample_scale = sample_scale(bpc);
    /* filter_and_downscale() resamples unless ALMOST_EQUAL(prescale, 1.0).
     * When it skips the resample but lround() still changed the plane size,
     * the reference filters memory beyond the copied picture; the device
     * resamples instead of reading outside its raw plane. */
    const bool identity = std::fabs(opt.speed_prescale - 1.0) < 1.0e-3;
    const bool same_size = g.scaled_w == g.src_w && g.scaled_h == g.src_h;
    g.prescale = (!identity || !same_size) ? 1 : 0;
    g.scale_method = method;
}

} // namespace

namespace
{

void fill_filters(const SpeedInternalOptions &opt, Filters &f)
{
    const auto kernelscale = static_cast<float>(opt.speed_kernelscale);
    std::memset(&f, 0, sizeof(f));
    f.antialias_width = static_cast<uint32_t>(vif_get_filter_size(1, kernelscale));
    speed_get_antialias_filter(f.antialias, SPEED_INTERNAL_NUM_SCALES, kernelscale);
    f.lowpass_width =
        static_cast<uint32_t>(vif_get_filter_size(SPEED_INTERNAL_NUM_SCALES, kernelscale));
    vif_get_filter(f.lowpass, SPEED_INTERNAL_NUM_SCALES, kernelscale);
}

void fill_scoring(const SpeedInternalOptions &opt, Scoring &s)
{
    const auto sigma_nn = static_cast<float>(opt.speed_sigma_nn);
    const auto nn_floor = static_cast<float>(opt.speed_nn_floor);
    s.sigma_nn = sigma_nn;
    s.entropy_constant = speed_internal_entropy_constant();
    s.base_entropy =
        speed_internal_base_entropy(static_cast<size_t>(kElements), sigma_nn, nn_floor);
    s.weight_mode = opt.speed_weight_var_mode;
}

} // namespace

int speed_sycl::configure(const SpeedInternalDimensions &dim, const SpeedInternalOptions &opt,
                          unsigned bpc, PipelineConfig &config)
{
    /* speed_init(), speed.c. */
    if (!vif_validate_kernelscale(static_cast<float>(opt.speed_kernelscale))) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "invalid speed_kernelscale\n");
        return -EINVAL;
    }
    enum vif_scaling_method method = vif_scale_nearest;
    if (vif_get_scaling_method(opt.speed_prescale_method, &method)) {
        return -EINVAL;
    }
    if (opt.speed_weight_var_mode < 0 || opt.speed_weight_var_mode > 6) {
        return -EINVAL;
    }
    fill_geometry(dim, opt, bpc, static_cast<int32_t>(method), config.geometry);
    fill_filters(opt, config.filters);
    fill_scoring(opt, config.scoring);
    return 0;
}
