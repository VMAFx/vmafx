/**
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Integer VIF feature extractor — HIP backend.
 *
 *  Direct port of core/src/feature/cuda/integer_vif_cuda.c.
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
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "libvmaf/picture.h"

#include "integer_vif.h"
#include "integer_vif_hip.h"

#include "../../hip/picture_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

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

    /* The half-resolution planes inside data_buf that scales 1..3 read: the
     * same addresses as buf.ref / buf.dis, which the kernels take as
     * uintptr_t, kept as pointers for the host-side launches. */
    void *rd_ref;
    void *rd_dis;
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
        .alias = "ssclz",
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

    VmafVifScoreSet output = {
        .single_precision_ratio = true,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    const unsigned scale_start = s->vif_skip_scale0 ? 1u : 0u;
    for (unsigned sc = 0; sc < 4u; ++sc) {
        output.scale[sc * 2u] = vif.scale[sc].num;
        output.scale[sc * 2u + 1u] = vif.scale[sc].den;
        if (sc >= scale_start) {
            output.score_num += vif.scale[sc].num;
            output.score_den += vif.scale[sc].den;
        }
    }
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "integer_vif_hip", &output,
                                VMAF_VIF_INTEGER_NAMES, index);
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

typedef struct VifHipKernelSlot {
    hipFunction_t *slot;
    const char *name;
} VifHipKernelSlot;

/* Load the kernel blob and resolve the ten kernels by name. On failure the
 * module is unloaded again and `s->module` is NULL. */
static int vif_hip_module_load(VifStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, vif_statistics_hsaco);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    const VifHipKernelSlot kernels[] = {
        {&s->func_vert_8_17_9, "filter1d_8_vertical_kernel_uint32_t_17_9"},
        {&s->func_hori_8_17_9, "filter1d_8_horizontal_kernel_2_17_9"},
        {&s->func_vert_16_17_9_0, "filter1d_16_vertical_kernel_uint2_17_9_0"},
        {&s->func_vert_16_9_5_1, "filter1d_16_vertical_kernel_uint2_9_5_1"},
        {&s->func_vert_16_5_3_2, "filter1d_16_vertical_kernel_uint2_5_3_2"},
        {&s->func_vert_16_3_0_3, "filter1d_16_vertical_kernel_uint2_3_0_3"},
        {&s->func_hori_16_17_9_0, "filter1d_16_horizontal_kernel_2_17_9_0"},
        {&s->func_hori_16_9_5_1, "filter1d_16_horizontal_kernel_2_9_5_1"},
        {&s->func_hori_16_5_3_2, "filter1d_16_horizontal_kernel_2_5_3_2"},
        {&s->func_hori_16_3_0_3, "filter1d_16_horizontal_kernel_2_3_0_3"},
    };
    const unsigned n_kernels = (unsigned)(sizeof(kernels) / sizeof(kernels[0]));
    for (unsigned i = 0; i < n_kernels && rc == hipSuccess; i++)
        rc = hipModuleGetFunction(kernels[i].slot, s->module, kernels[i].name);
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return vif_hip_err(rc);
}

/* 8-bpc scale 0 launch. */
static int vif_hip_filter1d_8(VifStateHip *s, uint8_t *ref_in, uint8_t *dis_in, int w, int h,
                              hipStream_t stream)
{
    const int BX_V = 32;
    const int BY_V = 8;
    const int GX_V = (w + BX_V - 1) / BX_V;
    const int GY_V = (h + BY_V - 1) / BY_V;

    /* The kernels take the VifBufferHip by value: args[0] points at it. */
    VifBufferHip *buf = &s->buf;
    /* ADR-0537: pass &vif_filt_dev (address of the variable storing
     * the device pointer), NOT the host-array address. */
    void *vif_filt_dev = s->vif_filt_dev;
    void *args_vert[] = {(void *)buf, (void *)&ref_in, (void *)&dis_in,
                         (void *)&w,  (void *)&h,      (void *)&vif_filt_dev};
    hipError_t rc =
        hipModuleLaunchKernel(s->func_vert_8_17_9, (unsigned)GX_V, (unsigned)GY_V, 1u,
                              (unsigned)BX_V, (unsigned)BY_V, 1u, 0u, stream, args_vert, NULL);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    const int BX_H = 128;
    const int GX_H = (w + BX_H - 1) / BX_H;
    const int GY_H = h;

    vif_accums_hip *accum_ptr = &((vif_accums_hip *)s->accum_dev)[0];
    void *args_hori[] = {(void *)buf,
                         (void *)&w,
                         (void *)&h,
                         (void *)&vif_filt_dev,
                         (void *)&s->vif_enhn_gain_limit,
                         (void *)&accum_ptr};
    rc = hipModuleLaunchKernel(s->func_hori_8_17_9, (unsigned)GX_H, (unsigned)GY_H, 1u,
                               (unsigned)BX_H, 1u, 1u, 0u, stream, args_hori, NULL);
    return vif_hip_err(rc);
}

/* The vertical / horizontal 16-bpc kernel pair of one scale. */
static int vif_hip_pick_16(const VifStateHip *s, int scale, hipFunction_t *vert_func,
                           hipFunction_t *hori_func)
{
    switch (scale) {
    case 0:
        *vert_func = s->func_vert_16_17_9_0;
        *hori_func = s->func_hori_16_17_9_0;
        return 0;
    case 1:
        *vert_func = s->func_vert_16_9_5_1;
        *hori_func = s->func_hori_16_9_5_1;
        return 0;
    case 2:
        *vert_func = s->func_vert_16_5_3_2;
        *hori_func = s->func_hori_16_5_3_2;
        return 0;
    case 3:
        *vert_func = s->func_vert_16_3_0_3;
        *hori_func = s->func_hori_16_3_0_3;
        return 0;
    default:
        return -EINVAL;
    }
}

/* 16-bpc launch — all four scales. */
static int vif_hip_filter1d_16(VifStateHip *s, uint16_t *ref_in, uint16_t *dis_in, int w, int h,
                               int scale, int bpc, hipStream_t stream)
{
    /* Scale 0 reads samples at the picture's bit depth; scales 1..3 read the
     * 16-bit planes the previous scale wrote. */
    const bool raw = (scale == 0);
    int32_t shift_HP = 16;
    int32_t add_shift_HP = 32768;
    int32_t shift_VP = raw ? bpc : 16;
    int32_t add_shift_VP = raw ? (1 << (bpc - 1)) : 32768;
    int32_t shift_VP_sq = raw ? ((bpc - 8) * 2) : 16;
    int32_t add_shift_VP_sq = 32768;
    if (raw)
        add_shift_VP_sq = (bpc == 8) ? 0 : 1 << (shift_VP_sq - 1);

    hipFunction_t vert_func = NULL;
    hipFunction_t hori_func = NULL;
    const int pick_err = vif_hip_pick_16(s, scale, &vert_func, &hori_func);
    if (pick_err != 0)
        return pick_err;

    const int BX_V = 32;
    const int BY_V = 8;
    const int GX_V = (w + BX_V - 1) / BX_V;
    const int GY_V = (h + BY_V - 1) / BY_V;

    VifBufferHip *buf = &s->buf;
    void *vif_filt_dev = s->vif_filt_dev;
    void *args_vert[] = {
        (void *)buf,          (void *)&ref_in,       (void *)&dis_in,   (void *)&w,
        (void *)&h,           (void *)&add_shift_VP, (void *)&shift_VP, (void *)&add_shift_VP_sq,
        (void *)&shift_VP_sq, (void *)&vif_filt_dev};
    hipError_t rc =
        hipModuleLaunchKernel(vert_func, (unsigned)GX_V, (unsigned)GY_V, 1u, (unsigned)BX_V,
                              (unsigned)BY_V, 1u, 0u, stream, args_vert, NULL);
    if (rc != hipSuccess)
        return vif_hip_err(rc);

    const int BX_H = 128;
    const int GX_H = (w + BX_H - 1) / BX_H;
    const int GY_H = h;

    vif_accums_hip *accum_ptr = &((vif_accums_hip *)s->accum_dev)[scale];
    void *args_hori[] = {(void *)buf,
                         (void *)&w,
                         (void *)&h,
                         (void *)&add_shift_HP,
                         (void *)&shift_HP,
                         (void *)&vif_filt_dev,
                         (void *)&s->vif_enhn_gain_limit,
                         (void *)&accum_ptr};
    rc = hipModuleLaunchKernel(hori_func, (unsigned)GX_H, (unsigned)GY_H, 1u, (unsigned)BX_H, 1u,
                               1u, 0u, stream, args_hori, NULL);
    return vif_hip_err(rc);
}

/* Private stream and the two events. On failure the handles already created
 * stay set; vif_hip_release() destroys them. */
static int vif_hip_stream_init(VifStateHip *s)
{
    hipError_t rc = hipStreamCreate(&s->str);
    if (rc == hipSuccess)
        rc = hipEventCreate(&s->submit);
    if (rc == hipSuccess)
        rc = hipEventCreate(&s->finished);
    return vif_hip_err(rc);
}

/* Byte strides of every plane, cache-line aligned. Returns the size of the
 * one device slab that holds the planes, and the half-resolution plane size
 * through `rd_size`. */
static size_t vif_hip_layout_strides(VifBufferHip *buf, unsigned w, unsigned h, unsigned bpc,
                                     size_t *rd_size)
{
    const int cache_line = 64;
    const ptrdiff_t bpp = (bpc > 8) ? 2 : 1;
    buf->stride = ((ptrdiff_t)w * bpp + cache_line - 1) / cache_line * cache_line;
    buf->rd_stride = (((ptrdiff_t)((w + 1) / 2) * 2) + cache_line - 1) / cache_line * cache_line;
    buf->stride_16 =
        (ptrdiff_t)(((w * sizeof(uint16_t)) + cache_line - 1) / cache_line * cache_line);
    buf->stride_32 =
        (ptrdiff_t)(((w * sizeof(uint32_t)) + cache_line - 1) / cache_line * cache_line);
    buf->stride_64 =
        (ptrdiff_t)(((w * sizeof(uint64_t)) + cache_line - 1) / cache_line * cache_line);
    buf->stride_tmp = buf->stride_32;

    *rd_size = (size_t)buf->rd_stride * ((h + 1) / 2);
    return 2u * *rd_size + 2u * ((size_t)h * (size_t)buf->stride_16) +
           5u * ((size_t)h * (size_t)buf->stride_32) + 8u * ((size_t)h * (size_t)buf->stride_tmp);
}

/* Carve the slab `data_buf` into the planes, in the order the CUDA twin lays
 * them out: two half-resolution planes, two 16-bit mu planes, five 32-bit
 * moment planes, eight 32-bit tmp planes. */
static void vif_hip_layout_planes(VifStateHip *s, size_t rd_size, unsigned h)
{
    VifBufferHip *buf = &s->buf;
    uint8_t *ptr = (uint8_t *)s->data_buf;
    s->rd_ref = ptr;
    buf->ref = (uintptr_t)ptr;
    ptr += rd_size;
    s->rd_dis = ptr;
    buf->dis = (uintptr_t)ptr;
    ptr += rd_size;

    const size_t plane_16 = (size_t)h * (size_t)buf->stride_16;
    buf->mu1 = (uint16_t *)ptr;
    ptr += plane_16;
    buf->mu2 = (uint16_t *)ptr;
    ptr += plane_16;

    const size_t plane_32 = (size_t)h * (size_t)buf->stride_32;
    uint32_t **moments[] = {&buf->mu1_32, &buf->mu2_32, &buf->ref_sq, &buf->dis_sq, &buf->ref_dis};
    for (unsigned i = 0; i < 5u; i++) {
        *moments[i] = (uint32_t *)ptr;
        ptr += plane_32;
    }

    const size_t plane_tmp = (size_t)h * (size_t)buf->stride_tmp;
    uint32_t **tmps[] = {&buf->tmp.mu1,        &buf->tmp.mu2,     &buf->tmp.ref,
                         &buf->tmp.dis,        &buf->tmp.ref_dis, &buf->tmp.ref_convol,
                         &buf->tmp.dis_convol, &buf->tmp.padding};
    for (unsigned i = 0; i < 8u; i++) {
        *tmps[i] = (uint32_t *)ptr;
        ptr += plane_tmp;
    }
}

/* Allocate the plane slab, the picture staging buffers (ADR-0537), the
 * accumulators and the filter-table buffer. On failure the buffers already
 * allocated stay set; vif_hip_release() frees them. */
static int vif_hip_bufs_alloc(VifStateHip *s, size_t data_sz, unsigned h)
{
    s->pic_dev_bytes = (size_t)s->buf.stride * (size_t)h;
    hipError_t rc = hipMalloc(&s->data_buf, data_sz);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->ref_in_dev, s->pic_dev_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->dis_in_dev, s->pic_dev_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->accum_dev, sizeof(vif_accums_hip) * 4u);
    if (rc == hipSuccess)
        rc = hipHostMalloc(&s->accum_host, sizeof(vif_accums_hip) * 4u, 0u);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->vif_filt_dev, sizeof(vif_filter1d_table));
    return (rc == hipSuccess) ? 0 : -ENOMEM;
}

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error. */
static int vif_hip_release(VifStateHip *s)
{
    int ret = 0;
    if (s->str != NULL)
        ret = vif_hip_err(hipStreamSynchronize(s->str));

    hipError_t rc = (s->accum_host != NULL) ? hipHostFree(s->accum_host) : hipSuccess;
    s->accum_host = NULL;
    if (ret == 0)
        ret = vif_hip_err(rc);

    void **dev_bufs[] = {&s->accum_dev, &s->ref_in_dev, &s->dis_in_dev, &s->data_buf,
                         &s->vif_filt_dev};
    for (unsigned i = 0; i < 5u; i++) {
        rc = (*dev_bufs[i] != NULL) ? hipFree(*dev_bufs[i]) : hipSuccess;
        *dev_bufs[i] = NULL;
        if (ret == 0)
            ret = vif_hip_err(rc);
    }
    s->rd_ref = NULL;
    s->rd_dis = NULL;

    const hipError_t rcs[] = {
        (s->module != NULL) ? hipModuleUnload(s->module) : hipSuccess,
        (s->finished != NULL) ? hipEventDestroy(s->finished) : hipSuccess,
        (s->submit != NULL) ? hipEventDestroy(s->submit) : hipSuccess,
        (s->str != NULL) ? hipStreamDestroy(s->str) : hipSuccess,
    };
    s->module = NULL;
    s->finished = NULL;
    s->submit = NULL;
    s->str = NULL;
    for (unsigned i = 0; i < 4u && ret == 0; i++)
        ret = vif_hip_err(rcs[i]);

    if (s->feature_name_dict != NULL) {
        const int e = vmaf_dictionary_free(&s->feature_name_dict);
        if (ret == 0)
            ret = e;
    }
    return ret;
}

#endif /* HAVE_HIPCC */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)bpc;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    VifStateHip *s = fex->priv;

    size_t rd_size = 0u;
    const size_t data_sz = vif_hip_layout_strides(&s->buf, w, h, bpc, &rd_size);

    int err = vif_hip_stream_init(s);
    if (err == 0)
        err = vif_hip_module_load(s);
    if (err == 0)
        err = vif_hip_bufs_alloc(s, data_sz, h);
    if (err == 0) {
        vif_hip_layout_planes(s, rd_size, h);
        /* ADR-0537: upload the host-side static `vif_filter1d_table` to a
         * device buffer (144 bytes). */
        if (hipMemcpy(s->vif_filt_dev, vif_filter1d_table, sizeof(vif_filter1d_table),
                      hipMemcpyHostToDevice) != hipSuccess)
            err = -EIO;
    }
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)vif_hip_release(s);
    return err;
#endif
}

#ifdef HAVE_HIPCC
/* Launch the four scales on the private stream. Scale 0 reads the staged
 * picture (ADR-0537); scales 1..3 read the half-resolution planes the
 * previous scale wrote. */
static int vif_hip_launch_scales(VifStateHip *s, unsigned w0, unsigned h0, unsigned bpc)
{
    int w = (int)w0;
    int h = (int)h0;
    int err = 0;
    for (unsigned scale = 0; scale < 4u && err == 0; ++scale) {
        if (scale > 0) {
            w /= 2;
            h /= 2;
        }
        if (bpc == 8u && scale == 0u) {
            err = vif_hip_filter1d_8(s, (uint8_t *)s->ref_in_dev, (uint8_t *)s->dis_in_dev, w, h,
                                     s->str);
        } else if (scale == 0u) {
            err = vif_hip_filter1d_16(s, (uint16_t *)s->ref_in_dev, (uint16_t *)s->dis_in_dev, w, h,
                                      (int)scale, (int)bpc, s->str);
        } else {
            err = vif_hip_filter1d_16(s, (uint16_t *)s->rd_ref, (uint16_t *)s->rd_dis, w, h,
                                      (int)scale, (int)bpc, s->str);
        }
    }
    return err;
}
#endif /* HAVE_HIPCC */

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

    /* ADR-0537: stage the host Y plane into device memory. Returns once both
     * pictures are read: the caller recycles them when submit() returns
     * (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    const ptrdiff_t bpp = (ref_pic->bpc > 8) ? 2 : 1;
    const size_t row_bytes = (size_t)ref_pic->w[0] * (size_t)bpp;
    const VmafHipPlaneUpload planes[] = {
        {.dst = s->ref_in_dev,
         .dst_pitch = (size_t)s->buf.stride,
         .pic = ref_pic,
         .plane = 0u,
         .row_bytes = row_bytes,
         .rows = ref_pic->h[0]},
        {.dst = s->dis_in_dev,
         .dst_pitch = (size_t)s->buf.stride,
         .pic = dist_pic,
         .plane = 0u,
         .row_bytes = row_bytes,
         .rows = dist_pic->h[0]},
    };
    int err = vmaf_hip_picture_upload(planes, 2u, vmaf_hip_stream_bits(s->str));
    if (err == 0)
        err = vif_hip_launch_scales(s, ref_pic->w[0], ref_pic->h[0], ref_pic->bpc);
    if (err != 0)
        return err;

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
    return vif_hip_release(fex->priv);
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

/* Declared via extern in feature_extractor.cpp's registry. */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
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

/* NOLINTEND(modernize-use-nullptr) */
