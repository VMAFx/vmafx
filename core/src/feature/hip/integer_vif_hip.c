/**
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Integer VIF feature extractor — HIP backend.
 *
 *  Direct port of libvmaf/src/feature/cuda/integer_vif_cuda.c.
 *  Call graph, struct layout, and score formula are preserved verbatim.
 *
 *  ADR-0537: filter table uploaded to device memory at init time and
 *  passed to every kernel launch as a device pointer.  The pre-fix code
 *  passed the address of the host-side `vif_filter1d_table` static array
 *  directly to `hipModuleLaunchKernel`, which the AMD GPU dereferenced
 *  and faulted on (GPU memory access fault on frame 0).
 *
 *  HIP adaptation notes:
 *    - CUdeviceptr / CUstream / CUevent  ->  uintptr_t / hipStream_t / hipEvent_t
 *    - cuModuleLoadData / cuModuleGetFunction / cuLaunchKernel
 *        ->  hipModuleLoadData / hipModuleGetFunction / hipModuleLaunchKernel
 *    - Accumulator memset: cuMemsetD8Async -> hipMemsetAsync
 *    - DtoH copy: cuMemcpyDtoHAsync -> hipMemcpyAsync (DeviceToHost)
 *    - Drain-batch fence (ADR-0242): not wired in HIP yet; the finished
 *      event is recorded and synchronised at collect() time.
 *    - HSACO fat binary embedded via xxd -i in the meson
 *      hip_hsaco_sources pipeline (same shape as ADR-0372 / psnr_score.hsaco).
 *
 *  Without HAVE_HIPCC, every lifecycle helper returns -ENOSYS so the
 *  feature engine falls through to the CPU path.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "integer_vif.h"
#include "integer_vif_hip.h"

#ifdef HAVE_HIPCC
#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

extern const unsigned char vif_statistics_hsaco[];
extern const unsigned int vif_statistics_hsaco_len;
#endif /* HAVE_HIPCC */

/* -------------------------------------------------------------------------
 * Private state
 * ------------------------------------------------------------------------- */

typedef struct VifStateHip {
    VifBufferHip buf;
    bool debug;
    bool vif_skip_scale0;
    double vif_enhn_gain_limit;
    VmafDictionary *feature_name_dict;

#ifdef HAVE_HIPCC
    hipStream_t str;
    hipEvent_t submit;
    hipEvent_t finished;
    hipModule_t module;

    hipFunction_t func_vert_8_17_9;
    hipFunction_t func_hori_8_17_9;
    hipFunction_t func_vert_16_17_9_0;
    hipFunction_t func_vert_16_9_5_1;
    hipFunction_t func_vert_16_5_3_2;
    hipFunction_t func_vert_16_3_0_3;
    hipFunction_t func_hori_16_17_9_0;
    hipFunction_t func_hori_16_9_5_1;
    hipFunction_t func_hori_16_5_3_2;
    hipFunction_t func_hori_16_3_0_3;

    void *accum_dev;
    void *accum_host;
    void *data_buf;

    /* Device buffer holding the 4x18 VIF filter table. ADR-0537. */
    void *vif_filt_dev;

    /* Device-side staging buffers for the host ref / dis pictures.
     * VmafPicture arrives as VMAF_PICTURE_BUFFER_TYPE_HOST; we copy the
     * Y plane into these per-frame via hipMemcpy2DAsync before launching
     * the scale-0 kernel.  Without this the kernel reads host memory and
     * the GPU faults (ADR-0537).  Same pattern as integer_motion_hip.c. */
    void *ref_in_dev;
    void *dis_in_dev;
    size_t pic_dev_bytes;
#endif /* HAVE_HIPCC */
} VifStateHip;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(VifStateHip, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "vif_enhn_gain_limit",
        .alias = "egl",
        .help = "enhancement gain imposed on vif, must be >= 1.0, "
                "where 1.0 means the gain is completely disabled",
        .offset = offsetof(VifStateHip, vif_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_VIF_ENHN_GAIN_LIMIT,
        .min = 1.0,
        .max = DEFAULT_VIF_ENHN_GAIN_LIMIT,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_skip_scale0",
        .help = "skip scale 0 (finest scale) VIF computation; "
                "score0 is forced to 0.0 (parity with CPU option)",
        .offset = offsetof(VifStateHip, vif_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

typedef struct {
    struct {
        float num;
        float den;
    } scale[4];
} VifScore;

#ifdef HAVE_HIPCC
static int write_scores_hip(VmafFeatureCollector *feature_collector, VifStateHip *s, unsigned index)
{
    VifScore vif;
    vif_accums_hip *accum = (vif_accums_hip *)s->accum_host;

    for (unsigned sc = 0; sc < 4; ++sc) {
        vif.scale[sc].num =
            (float)(accum[sc].num_log / 2048.0 + accum[sc].x2 +
                    (accum[sc].den_non_log - ((double)accum[sc].num_non_log / 16384.0) / 65025.0));
        vif.scale[sc].den =
            (float)(accum[sc].den_log / 2048.0 - (double)(accum[sc].x + (accum[sc].num_x * 17)) +
                    accum[sc].den_non_log);
    }

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_vif_scale0_score",
        s->vif_skip_scale0 ? 0.0 : vif.scale[0].num / vif.scale[0].den, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_vif_scale1_score",
                                                   vif.scale[1].num / vif.scale[1].den, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_vif_scale2_score",
                                                   vif.scale[2].num / vif.scale[2].den, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_vif_scale3_score",
                                                   vif.scale[3].num / vif.scale[3].den, index);

    if (!s->debug)
        return err;

    const double score_num = (s->vif_skip_scale0 ? 0.0 : (double)vif.scale[0].num) +
                             (double)vif.scale[1].num + (double)vif.scale[2].num +
                             (double)vif.scale[3].num;
    const double score_den = (s->vif_skip_scale0 ? 0.0 : (double)vif.scale[0].den) +
                             (double)vif.scale[1].den + (double)vif.scale[2].den +
                             (double)vif.scale[3].den;
    const double score = (score_den == 0.0) ? 1.0 : score_num / score_den;

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif", score, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_num", score_num, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_den", score_den, index);
    if (s->vif_skip_scale0) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_num_scale0", 0.0, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_den_scale0", -1.0, index);
    } else {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_num_scale0", vif.scale[0].num,
                                                       index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_den_scale0", vif.scale[0].den,
                                                       index);
    }
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "integer_vif_num_scale1", vif.scale[1].num, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "integer_vif_den_scale1", vif.scale[1].den, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "integer_vif_num_scale2", vif.scale[2].num, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "integer_vif_den_scale2", vif.scale[2].den, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "integer_vif_num_scale3", vif.scale[3].num, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "integer_vif_den_scale3", vif.scale[3].den, index);

    return err;
}
#endif /* HAVE_HIPCC */

#ifdef HAVE_HIPCC

static int vif_hip_err(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

static int vif_hip_module_load(VifStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, vif_statistics_hsaco);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

#define LOAD_FUNC(field, name)                                                                     \
    rc = hipModuleGetFunction(&s->field, s->module, name);                                         \
    if (rc != hipSuccess) {                                                                        \
        (void)hipModuleUnload(s->module);                                                          \
        s->module = NULL;                                                                          \
        return vif_hip_err(rc);                                                                    \
    }

    LOAD_FUNC(func_vert_8_17_9, "filter1d_8_vertical_kernel_uint32_t_17_9")
    LOAD_FUNC(func_hori_8_17_9, "filter1d_8_horizontal_kernel_2_17_9")
    LOAD_FUNC(func_vert_16_17_9_0, "filter1d_16_vertical_kernel_uint2_17_9_0")
    LOAD_FUNC(func_vert_16_9_5_1, "filter1d_16_vertical_kernel_uint2_9_5_1")
    LOAD_FUNC(func_vert_16_5_3_2, "filter1d_16_vertical_kernel_uint2_5_3_2")
    LOAD_FUNC(func_vert_16_3_0_3, "filter1d_16_vertical_kernel_uint2_3_0_3")
    LOAD_FUNC(func_hori_16_17_9_0, "filter1d_16_horizontal_kernel_2_17_9_0")
    LOAD_FUNC(func_hori_16_9_5_1, "filter1d_16_horizontal_kernel_2_9_5_1")
    LOAD_FUNC(func_hori_16_5_3_2, "filter1d_16_horizontal_kernel_2_5_3_2")
    LOAD_FUNC(func_hori_16_3_0_3, "filter1d_16_horizontal_kernel_2_3_0_3")
#undef LOAD_FUNC

    return 0;
}

/* 8-bpc scale 0 launch. */
static int vif_hip_filter1d_8(VifStateHip *s, uint8_t *ref_in, uint8_t *dis_in, int w, int h,
                              hipStream_t stream)
{
    const int BX_V = 32, BY_V = 8;
    const int GX_V = (w + BX_V - 1) / BX_V;
    const int GY_V = (h + BY_V - 1) / BY_V;

    VifBufferHip *buf = &s->buf;
    /* ADR-0537: pass &vif_filt_dev (address of the variable storing
     * the device pointer), NOT the host-array address. */
    void *vif_filt_dev = s->vif_filt_dev;
    void *args_vert[] = {buf, &ref_in, &dis_in, &w, &h, &vif_filt_dev};
    hipError_t rc =
        hipModuleLaunchKernel(s->func_vert_8_17_9, (unsigned)GX_V, (unsigned)GY_V, 1u,
                              (unsigned)BX_V, (unsigned)BY_V, 1u, 0u, stream, args_vert, NULL);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    const int BX_H = 128;
    const int GX_H = (w + BX_H - 1) / BX_H;
    const int GY_H = h;

    vif_accums_hip *accum_ptr = &((vif_accums_hip *)s->accum_dev)[0];
    void *args_hori[] = {buf, &w, &h, &vif_filt_dev, &s->vif_enhn_gain_limit, &accum_ptr};
    rc = hipModuleLaunchKernel(s->func_hori_8_17_9, (unsigned)GX_H, (unsigned)GY_H, 1u,
                               (unsigned)BX_H, 1u, 1u, 0u, stream, args_hori, NULL);
    return vif_hip_err(rc);
}

/* 16-bpc launch — all four scales. */
static int vif_hip_filter1d_16(VifStateHip *s, uint16_t *ref_in, uint16_t *dis_in, int w, int h,
                               int scale, int bpc, hipStream_t stream)
{
    int32_t add_shift_VP, shift_VP, add_shift_VP_sq, shift_VP_sq;
    int32_t add_shift_HP, shift_HP;

    if (scale == 0) {
        shift_HP = 16;
        add_shift_HP = 32768;
        shift_VP = bpc;
        add_shift_VP = 1 << (bpc - 1);
        shift_VP_sq = (bpc - 8) * 2;
        add_shift_VP_sq = (bpc == 8) ? 0 : 1 << (shift_VP_sq - 1);
    } else {
        shift_HP = 16;
        add_shift_HP = 32768;
        shift_VP = 16;
        add_shift_VP = 32768;
        shift_VP_sq = 16;
        add_shift_VP_sq = 32768;
    }

    hipFunction_t vert_func;
    hipFunction_t hori_func;
    switch (scale) {
    case 0:
        vert_func = s->func_vert_16_17_9_0;
        hori_func = s->func_hori_16_17_9_0;
        break;
    case 1:
        vert_func = s->func_vert_16_9_5_1;
        hori_func = s->func_hori_16_9_5_1;
        break;
    case 2:
        vert_func = s->func_vert_16_5_3_2;
        hori_func = s->func_hori_16_5_3_2;
        break;
    case 3:
        vert_func = s->func_vert_16_3_0_3;
        hori_func = s->func_hori_16_3_0_3;
        break;
    default:
        return -EINVAL;
    }

    const int BX_V = 32, BY_V = 8;
    const int GX_V = (w + BX_V - 1) / BX_V;
    const int GY_V = (h + BY_V - 1) / BY_V;

    VifBufferHip *buf = &s->buf;
    void *vif_filt_dev = s->vif_filt_dev;
    void *args_vert[] = {buf,           &ref_in,   &dis_in,          &w,           &h,
                         &add_shift_VP, &shift_VP, &add_shift_VP_sq, &shift_VP_sq, &vif_filt_dev};
    hipError_t rc =
        hipModuleLaunchKernel(vert_func, (unsigned)GX_V, (unsigned)GY_V, 1u, (unsigned)BX_V,
                              (unsigned)BY_V, 1u, 0u, stream, args_vert, NULL);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    const int BX_H = 128;
    const int GX_H = (w + BX_H - 1) / BX_H;
    const int GY_H = h;

    vif_accums_hip *accum_ptr = &((vif_accums_hip *)s->accum_dev)[scale];
    void *args_hori[] = {
        buf, &w, &h, &add_shift_HP, &shift_HP, &vif_filt_dev, &s->vif_enhn_gain_limit, &accum_ptr};
    rc = hipModuleLaunchKernel(hori_func, (unsigned)GX_H, (unsigned)GY_H, 1u, (unsigned)BX_H, 1u,
                               1u, 0u, stream, args_hori, NULL);
    return vif_hip_err(rc);
}

#endif /* HAVE_HIPCC */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    VifStateHip *s = fex->priv;

    hipError_t rc = hipStreamCreate(&s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);
    rc = hipEventCreate(&s->submit);
    if (rc != hipSuccess)
        goto fail_stream;
    rc = hipEventCreate(&s->finished);
    if (rc != hipSuccess)
        goto fail_submit;

    int err = vif_hip_module_load(s);
    if (err != 0)
        goto fail_finished;

    const int cache_line = 64;
    const ptrdiff_t bpp = (bpc > 8) ? 2 : 1;
    s->buf.stride = ((ptrdiff_t)w * bpp + cache_line - 1) / cache_line * cache_line;
    s->buf.rd_stride = (((ptrdiff_t)((w + 1) / 2) * 2) + cache_line - 1) / cache_line * cache_line;
    s->buf.stride_16 =
        (ptrdiff_t)(((w * sizeof(uint16_t)) + cache_line - 1) / cache_line * cache_line);
    s->buf.stride_32 =
        (ptrdiff_t)(((w * sizeof(uint32_t)) + cache_line - 1) / cache_line * cache_line);
    s->buf.stride_64 =
        (ptrdiff_t)(((w * sizeof(uint64_t)) + cache_line - 1) / cache_line * cache_line);
    s->buf.stride_tmp = s->buf.stride_32;

    const size_t rd_size = (size_t)s->buf.rd_stride * ((h + 1) / 2);
    const size_t data_sz = 2u * rd_size + 2u * ((size_t)h * (size_t)s->buf.stride_16) +
                           5u * ((size_t)h * (size_t)s->buf.stride_32) +
                           8u * ((size_t)h * (size_t)s->buf.stride_tmp);

    rc = hipMalloc(&s->data_buf, data_sz);
    if (rc != hipSuccess) {
        err = -ENOMEM;
        goto fail_module;
    }

    uint8_t *ptr = (uint8_t *)s->data_buf;
    s->buf.ref = (uintptr_t)ptr;
    ptr += rd_size;
    s->buf.dis = (uintptr_t)ptr;
    ptr += rd_size;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.mu1 = (uint16_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_16;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.mu2 = (uint16_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_16;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.mu1_32 = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_32;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.mu2_32 = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_32;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.ref_sq = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_32;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.dis_sq = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_32;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.ref_dis = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_32;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.mu1 = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.mu2 = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.ref = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.dis = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.ref_dis = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.ref_convol = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.dis_convol = (uint32_t *)ptr;
    ptr += (size_t)h * (size_t)s->buf.stride_tmp;
    /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
    s->buf.tmp.padding = (uint32_t *)ptr;

    /* ADR-0537: per-frame host->device staging for the input picture. */
    s->pic_dev_bytes = (size_t)s->buf.stride * (size_t)h;
    rc = hipMalloc(&s->ref_in_dev, s->pic_dev_bytes);
    if (rc != hipSuccess) {
        err = -ENOMEM;
        goto fail_data;
    }
    rc = hipMalloc(&s->dis_in_dev, s->pic_dev_bytes);
    if (rc != hipSuccess) {
        err = -ENOMEM;
        goto fail_ref_in_dev;
    }

    rc = hipMalloc(&s->accum_dev, sizeof(vif_accums_hip) * 4u);
    if (rc != hipSuccess) {
        err = -ENOMEM;
        goto fail_dis_in_dev;
    }

    rc = hipHostMalloc(&s->accum_host, sizeof(vif_accums_hip) * 4u, 0u);
    if (rc != hipSuccess) {
        err = -ENOMEM;
        goto fail_accum_dev;
    }

    /* ADR-0537: upload the host-side static `vif_filter1d_table` to a
     * device buffer (144 bytes). */
    rc = hipMalloc(&s->vif_filt_dev, sizeof(vif_filter1d_table));
    if (rc != hipSuccess) {
        err = -ENOMEM;
        goto fail_accum_host;
    }
    rc = hipMemcpy(s->vif_filt_dev, vif_filter1d_table, sizeof(vif_filter1d_table),
                   hipMemcpyHostToDevice);
    if (rc != hipSuccess) {
        err = -EIO;
        goto fail_filt_dev;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        err = -ENOMEM;
        goto fail_filt_dev;
    }

    return 0;

fail_filt_dev:
    (void)hipFree(s->vif_filt_dev);
    s->vif_filt_dev = NULL;
fail_accum_host:
    (void)hipHostFree(s->accum_host);
    s->accum_host = NULL;
fail_accum_dev:
    (void)hipFree(s->accum_dev);
    s->accum_dev = NULL;
fail_dis_in_dev:
    (void)hipFree(s->dis_in_dev);
    s->dis_in_dev = NULL;
fail_ref_in_dev:
    (void)hipFree(s->ref_in_dev);
    s->ref_in_dev = NULL;
fail_data:
    (void)hipFree(s->data_buf);
    s->data_buf = NULL;
fail_module:
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
fail_finished:
    (void)hipEventDestroy(s->finished);
fail_submit:
    (void)hipEventDestroy(s->submit);
fail_stream:
    (void)hipStreamDestroy(s->str);
    return err;
#endif
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    return -ENOSYS;
#else
    VifStateHip *s = fex->priv;

    hipError_t rc = hipMemsetAsync(s->accum_dev, 0, sizeof(vif_accums_hip) * 4u, s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    /* ADR-0537: stage the host Y plane into device memory. */
    const ptrdiff_t bpp = (ref_pic->bpc > 8) ? 2 : 1;
    const size_t row_bytes = (size_t)ref_pic->w[0] * (size_t)bpp;
    rc = hipMemcpy2DAsync(s->ref_in_dev, (size_t)s->buf.stride, ref_pic->data[0],
                          (size_t)ref_pic->stride[0], row_bytes, (size_t)ref_pic->h[0],
                          hipMemcpyHostToDevice, s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);
    rc = hipMemcpy2DAsync(s->dis_in_dev, (size_t)s->buf.stride, dist_pic->data[0],
                          (size_t)dist_pic->stride[0], row_bytes, (size_t)dist_pic->h[0],
                          hipMemcpyHostToDevice, s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    int w = (int)ref_pic->w[0];
    int h = (int)ref_pic->h[0];

    for (unsigned scale = 0; scale < 4u; ++scale) {
        if (scale > 0) {
            w /= 2;
            h /= 2;
        }

        int err = 0;
        if (ref_pic->bpc == 8u && scale == 0u) {
            /* ADR-0537: device staging buffers (host pic was copied above). */
            err = vif_hip_filter1d_8(s, (uint8_t *)s->ref_in_dev, (uint8_t *)s->dis_in_dev, w, h,
                                     s->str);
        } else if (scale == 0u) {
            err = vif_hip_filter1d_16(s, (uint16_t *)s->ref_in_dev, (uint16_t *)s->dis_in_dev, w, h,
                                      (int)scale, (int)ref_pic->bpc, s->str);
        } else {
            /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
            err = vif_hip_filter1d_16(s, (uint16_t *)s->buf.ref, (uint16_t *)s->buf.dis, w, h,
                                      (int)scale, (int)ref_pic->bpc, s->str);
        }
        if (err != 0)
            return err;
    }

    rc = hipMemcpyAsync(s->accum_host, s->accum_dev, sizeof(vif_accums_hip) * 4u,
                        hipMemcpyDeviceToHost, s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    rc = hipEventRecord(s->finished, s->str);
    return vif_hip_err(rc);
#endif
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    VifStateHip *s = fex->priv;

    hipError_t rc = hipStreamSynchronize(s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    return write_scores_hip(feature_collector, s, index);
#endif
}

static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
#ifndef HAVE_HIPCC
    (void)fex;
    return -ENOSYS;
#else
    VifStateHip *s = fex->priv;
    hipError_t rc = hipStreamSynchronize(s->str);
    if (rc != hipSuccess)
        return vif_hip_err(rc);
    return 1;
#endif
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
#ifndef HAVE_HIPCC
    (void)fex;
    return -ENOSYS;
#else
    VifStateHip *s = fex->priv;
    int ret = 0;

    hipError_t rc = hipStreamSynchronize(s->str);
    if (rc != hipSuccess)
        ret = vif_hip_err(rc);

    if (s->accum_host != NULL) {
        rc = hipHostFree(s->accum_host);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->accum_host = NULL;
    }
    if (s->accum_dev != NULL) {
        rc = hipFree(s->accum_dev);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->accum_dev = NULL;
    }
    if (s->ref_in_dev != NULL) {
        rc = hipFree(s->ref_in_dev);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->ref_in_dev = NULL;
    }
    if (s->dis_in_dev != NULL) {
        rc = hipFree(s->dis_in_dev);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->dis_in_dev = NULL;
    }
    if (s->data_buf != NULL) {
        rc = hipFree(s->data_buf);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->data_buf = NULL;
    }
    if (s->vif_filt_dev != NULL) {
        rc = hipFree(s->vif_filt_dev);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->vif_filt_dev = NULL;
    }
    if (s->module != NULL) {
        rc = hipModuleUnload(s->module);
        if (rc != hipSuccess && ret == 0)
            ret = vif_hip_err(rc);
        s->module = NULL;
    }
    rc = hipEventDestroy(s->finished);
    if (rc != hipSuccess && ret == 0)
        ret = vif_hip_err(rc);
    rc = hipEventDestroy(s->submit);
    if (rc != hipSuccess && ret == 0)
        ret = vif_hip_err(rc);
    rc = hipStreamDestroy(s->str);
    if (rc != hipSuccess && ret == 0)
        ret = vif_hip_err(rc);

    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    return ret;
#endif
}

static const char *provided_features[] = {
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
    "integer_vif",
    "integer_vif_num",
    "integer_vif_den",
    "integer_vif_num_scale0",
    "integer_vif_den_scale0",
    "integer_vif_num_scale1",
    "integer_vif_den_scale1",
    "integer_vif_num_scale2",
    "integer_vif_den_scale2",
    "integer_vif_num_scale3",
    "integer_vif_den_scale3",
    NULL,
};

VmafFeatureExtractor vmaf_fex_integer_vif_hip = {
    .name = "vif_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .flush = flush_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(VifStateHip),
    .provided_features = provided_features,
    /* ADR-0537: re-enabled after the kernel-level fix (filter table
     * uploaded to device, kernel half-widths corrected, downsample
     * write path added).  ADR-0530 cleared this flag pending the fix. */
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
};
