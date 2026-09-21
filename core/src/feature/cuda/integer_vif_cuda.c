/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "cpu.h"
#include "common/macros.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"

#include "picture.h"
#include "cuda/integer_vif_cuda.h"
#include "drain_batch.h"
#include "picture_cuda.h"

#if ARCH_X86
#include "x86/vif_avx2.h"
#if HAVE_AVX512
#include "x86/vif_avx512.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif
#endif

typedef struct VifStateCuda {
    VifBufferCuda buf;
    CUevent event, finished;
    CUstream str;
    /* Engine-scope fence batching opt-in flag (T-GPU-OPT-1, ADR-0242). */
    bool drained;
    bool debug;
    bool enable_chroma;
    unsigned n_planes;
    double vif_enhn_gain_limit;
    bool vif_skip_scale0;
    void (*filter1d_8)(VifBufferCuda *buf, uint8_t *ref_in, uint8_t *dis_in, unsigned w, unsigned h,
                       double vif_enhn_gain_limit, CUstream stream);
    void (*filter1d_16)(VifBufferCuda *buf, uint16_t *ref_in, uint16_t *dis_in, unsigned w,
                        unsigned h, int scale, int bpc, double vif_enhn_gain_limit,
                        CUstream stream);
    VmafDictionary *feature_name_dict;
    CUfunction func_filter1d_8_vertical_kernel_uint32_t_17_9,
        func_filter1d_8_horizontal_kernel_2_17_9, func_filter1d_16_vertical_kernel_uint2_17_9_0,
        func_filter1d_16_vertical_kernel_uint2_9_5_1, func_filter1d_16_vertical_kernel_uint2_5_3_2,
        func_filter1d_16_vertical_kernel_uint2_3_0_3, func_filter1d_16_horizontal_kernel_2_17_9_0,
        func_filter1d_16_horizontal_kernel_2_9_5_1, func_filter1d_16_horizontal_kernel_2_5_3_2,
        func_filter1d_16_horizontal_kernel_2_3_0_3;
    /* PTX module backing the VIF filter kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule filter1d_module;
} VifStateCuda;

typedef struct write_score_parameters_vif {
    VmafFeatureCollector *feature_collector;
    VifStateCuda *s;
    unsigned index;
} write_score_parameters_vif;

static const VmafOption options[] = {{
                                         .name = "debug",
                                         .help = "debug mode: enable additional output",
                                         .offset = offsetof(VifStateCuda, debug),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                     },
                                     {
                                         /* Vestigial no-op retained for backward compatibility
                                          * with callers that pass `integer_vif:enable_chroma=…`
                                          * on the CLI or in a model JSON.  VIF is luma-only
                                          * across every backend (CPU, CUDA, HIP, SYCL, Vulkan,
                                          * Metal) and across upstream Netflix/vmaf — see
                                          * ADR-0541.  Setting `enable_chroma=true` emits a
                                          * one-shot warning during init() and otherwise has
                                          * no effect on the produced scores. */
                                         .name = "enable_chroma",
                                         .help =
                                             "no-op (luma-only kernel; ADR-0541). retained for "
                                             "backward-compat with callers that set the option; "
                                             "emits a one-shot warning when true",
                                         .offset = offsetof(VifStateCuda, enable_chroma),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "vif_enhn_gain_limit",
                                         .alias = "egl",
                                         .help = "enhancement gain imposed on vif, must be >= 1.0, "
                                                 "where 1.0 means the gain is completely disabled",
                                         .offset = offsetof(VifStateCuda, vif_enhn_gain_limit),
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
                                         .offset = offsetof(VifStateCuda, vif_skip_scale0),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {0}};

static int vif_cuda_status(CUresult result, const char *operation)
{
    if (result == CUDA_SUCCESS)
        return 0;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_vif_cuda: %s failed with CUDA error %d\n", operation,
             (int)result);
    return vmaf_cuda_result_to_errno((int)result);
}

static void keep_first_vif_error(int *first_error, int error)
{
    if (!*first_error && error)
        *first_error = error;
}

static void record_vif_error(int *first_error, int error, const char *operation)
{
    if (!error)
        return;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_vif_cuda: %s failed with error %d\n", operation, error);
    keep_first_vif_error(first_error, error);
}

static int resolve_vif_functions(VifStateCuda *s, CudaFunctions *cu_f)
{
    CUfunction *const functions[] = {
        &s->func_filter1d_8_vertical_kernel_uint32_t_17_9,
        &s->func_filter1d_8_horizontal_kernel_2_17_9,
        &s->func_filter1d_16_vertical_kernel_uint2_17_9_0,
        &s->func_filter1d_16_vertical_kernel_uint2_9_5_1,
        &s->func_filter1d_16_vertical_kernel_uint2_5_3_2,
        &s->func_filter1d_16_vertical_kernel_uint2_3_0_3,
        &s->func_filter1d_16_horizontal_kernel_2_17_9_0,
        &s->func_filter1d_16_horizontal_kernel_2_9_5_1,
        &s->func_filter1d_16_horizontal_kernel_2_5_3_2,
        &s->func_filter1d_16_horizontal_kernel_2_3_0_3,
    };
    static const char *const names[] = {
        "filter1d_8_vertical_kernel_uint32_t_17_9", "filter1d_8_horizontal_kernel_2_17_9",
        "filter1d_16_vertical_kernel_uint2_17_9_0", "filter1d_16_vertical_kernel_uint2_9_5_1",
        "filter1d_16_vertical_kernel_uint2_5_3_2",  "filter1d_16_vertical_kernel_uint2_3_0_3",
        "filter1d_16_horizontal_kernel_2_17_9_0",   "filter1d_16_horizontal_kernel_2_9_5_1",
        "filter1d_16_horizontal_kernel_2_5_3_2",    "filter1d_16_horizontal_kernel_2_3_0_3",
    };

    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
        const int err = vif_cuda_status(
            cu_f->cuModuleGetFunction(functions[i], s->filter1d_module, names[i]), names[i]);
        if (err)
            return err;
    }
    return 0;
}

static int load_vif_runtime(VmafFeatureExtractor *fex)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = vif_cuda_status(cu_f->cuCtxPushCurrent(fex->cu_state->ctx), "cuCtxPushCurrent");
    if (err)
        return err;

    keep_first_vif_error(
        &err, vif_cuda_status(cu_f->cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0),
                              "cuStreamCreateWithPriority"));
    if (!err)
        keep_first_vif_error(&err, vif_cuda_status(cu_f->cuEventCreate(&s->event, CU_EVENT_DEFAULT),
                                                   "cuEventCreate(event)"));
    if (!err)
        keep_first_vif_error(&err,
                             vif_cuda_status(cu_f->cuEventCreate(&s->finished, CU_EVENT_DEFAULT),
                                             "cuEventCreate(finished)"));
    if (!err)
        keep_first_vif_error(
            &err, vif_cuda_status(cu_f->cuModuleLoadData(&s->filter1d_module, filter1d_ptx),
                                  "cuModuleLoadData"));
    if (!err)
        keep_first_vif_error(&err, resolve_vif_functions(s, cu_f));
    keep_first_vif_error(&err, vif_cuda_status(cu_f->cuCtxPopCurrent(NULL), "cuCtxPopCurrent"));
    return err;
}

static int release_vif_runtime(VmafFeatureExtractor *fex, bool synchronize)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = 0;
    if (synchronize && s->str)
        keep_first_vif_error(
            &err, vif_cuda_status(cu_f->cuStreamSynchronize(s->str), "cuStreamSynchronize"));
    if (s->str) {
        keep_first_vif_error(&err,
                             vif_cuda_status(cu_f->cuStreamDestroy(s->str), "cuStreamDestroy"));
        s->str = 0;
    }
    if (s->event) {
        keep_first_vif_error(&err,
                             vif_cuda_status(cu_f->cuEventDestroy(s->event), "cuEventDestroy"));
        s->event = 0;
    }
    if (s->finished) {
        keep_first_vif_error(
            &err, vif_cuda_status(cu_f->cuEventDestroy(s->finished), "cuEventDestroy(finished)"));
        s->finished = 0;
    }
    if (s->filter1d_module) {
        keep_first_vif_error(
            &err, vif_cuda_status(cu_f->cuModuleUnload(s->filter1d_module), "cuModuleUnload"));
        s->filter1d_module = NULL;
    }
    return err;
}

static int release_vif_buffers(VmafFeatureExtractor *fex)
{
    VifStateCuda *s = fex->priv;
    int err = 0;
    if (s->buf.data) {
        record_vif_error(&err, vmaf_cuda_buffer_free(fex->cu_state, s->buf.data), "free data");
        free(s->buf.data);
        s->buf.data = NULL;
    }
    if (s->buf.accum_data) {
        record_vif_error(&err, vmaf_cuda_buffer_free(fex->cu_state, s->buf.accum_data),
                         "free accumulator");
        free(s->buf.accum_data);
        s->buf.accum_data = NULL;
    }
    if (s->buf.accum_host) {
        record_vif_error(&err, vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.accum_host),
                         "free host accumulator");
        s->buf.accum_host = NULL;
    }
    record_vif_error(&err, vmaf_dictionary_free(&s->feature_name_dict), "dictionary free");
    return err;
}

static int release_vif_resources(VmafFeatureExtractor *fex, bool synchronize)
{
    int err = release_vif_runtime(fex, synchronize);
    keep_first_vif_error(&err, release_vif_buffers(fex));
    return err;
}

static void configure_vif_luma(VifStateCuda *s, enum VmafPixelFormat pix_fmt)
{
    if (s->enable_chroma) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_vif (CUDA): enable_chroma=true requested but VIF is "
                 "luma-only by design (matches CPU integer_vif and upstream "
                 "Netflix/vmaf); option is a no-op. See ADR-0541.\n");
        s->enable_chroma = false;
    }
    (void)pix_fmt;
    s->n_planes = 1;
}

static int configure_vif_layout(VmafFeatureExtractor *fex, unsigned bpc, unsigned w, unsigned h,
                                size_t *rd_size, size_t *data_size)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int texture_alignment = 0;
    CHECK_CUDA_RETURN(cu_f, cuDeviceGetAttribute(&texture_alignment,
                                                 CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT,
                                                 fex->cu_state->dev));

    const bool hbd = bpc > 8;
    s->buf.stride =
        texture_alignment * ((w * (1 << (int)hbd) + texture_alignment - 1) / texture_alignment);
    const int rd_w_bytes = ((w + 1) / 2) * sizeof(uint16_t);
    s->buf.rd_stride =
        texture_alignment * ((rd_w_bytes + texture_alignment - 1) / texture_alignment);
    s->buf.stride_16 = ALIGN_CEIL(w * sizeof(uint16_t));
    s->buf.stride_32 = ALIGN_CEIL(w * sizeof(uint32_t));
    s->buf.stride_64 = ALIGN_CEIL(w * sizeof(uint64_t));
    s->buf.stride_tmp = ALIGN_CEIL(w * sizeof(uint32_t));
    *rd_size = s->buf.rd_stride * ((h + 1) / 2);
    *data_size = 2 * *rd_size + 2 * (h * s->buf.stride_16) + 5 * (h * s->buf.stride_32) +
                 8 * (s->buf.stride_tmp * h);
    return 0;
}

static int allocate_vif_buffers(VmafFeatureExtractor *fex, size_t data_size)
{
    VifStateCuda *s = fex->priv;
    int err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.data, data_size);
    if (!err)
        err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.accum_data, sizeof(vif_accums) * 4);
    if (!err) {
        err = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->buf.accum_host,
                                          sizeof(vif_accums) * 4);
    }
    if (err)
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_vif_cuda: allocation failed with error %d\n", err);
    return err;
}

static void store_vif_device_pointer(void *destination, CUdeviceptr address)
{
    _Static_assert(sizeof(CUdeviceptr) == sizeof(void *), "CUDA device pointer width mismatch");
    memcpy(destination, &address, sizeof(address));
}

static int layout_vif_buffers(VmafFeatureExtractor *fex, size_t rd_size, unsigned h)
{
    VifStateCuda *s = fex->priv;
    CUdeviceptr data = 0;
    int err = vmaf_cuda_buffer_get_dptr(s->buf.data, &data);
    if (err)
        return err;

    s->buf.ref = data;
    data += rd_size;
    s->buf.dis = data;
    data += rd_size;
#define VIF_REGION(field, bytes)                                                                   \
    do {                                                                                           \
        store_vif_device_pointer(&(field), data);                                                  \
        data += (bytes);                                                                           \
    } while (0)
    VIF_REGION(s->buf.mu1, h * s->buf.stride_16);
    VIF_REGION(s->buf.mu2, h * s->buf.stride_16);
    VIF_REGION(s->buf.mu1_32, h * s->buf.stride_32);
    VIF_REGION(s->buf.mu2_32, h * s->buf.stride_32);
    VIF_REGION(s->buf.ref_sq, h * s->buf.stride_32);
    VIF_REGION(s->buf.dis_sq, h * s->buf.stride_32);
    VIF_REGION(s->buf.ref_dis, h * s->buf.stride_32);
    VIF_REGION(s->buf.tmp.mu1, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.mu2, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.ref, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.dis, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.ref_dis, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.ref_convol, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.dis_convol, s->buf.stride_tmp * h);
    VIF_REGION(s->buf.tmp.padding, s->buf.stride_tmp * h);
#undef VIF_REGION

    CUdeviceptr accum = 0;
    err = vmaf_cuda_buffer_get_dptr(s->buf.accum_data, &accum);
    if (!err)
        store_vif_device_pointer(&s->buf.accum, accum);
    return err;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    VifStateCuda *s = fex->priv;
    configure_vif_luma(s, pix_fmt);
    int err = load_vif_runtime(fex);
    size_t rd_size = 0;
    size_t data_size = 0;
    if (!err)
        err = configure_vif_layout(fex, bpc, w, h, &rd_size, &data_size);
    if (!err)
        err = allocate_vif_buffers(fex, data_size);
    if (!err)
        err = layout_vif_buffers(fex, rd_size, h);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        const int cleanup_err = release_vif_resources(fex, false);
        if (cleanup_err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "integer_vif_cuda: cleanup after init failure also failed with error %d\n",
                     cleanup_err);
        }
    }
    return err;
}

static int filter1d_8(VifStateCuda *s, VifBufferCuda *buf, uint8_t *ref_in, uint8_t *dis_in, int w,
                      int h, double vif_enhn_gain_limit, CudaFunctions *cu_f, CUstream stream)
{
    {

        const int size_of_alignment_type = sizeof(uint32_t);
        const int BLOCKX = 128 / size_of_alignment_type;
        const int BLOCKY = 128 / (VMAF_CUDA_CACHE_LINE_SIZE / size_of_alignment_type);
        void *args_vert[] = {&*buf, &ref_in, &dis_in, &w, &h, (uint16_t *)&vif_filter1d_table};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_filter1d_8_vertical_kernel_uint32_t_17_9,
                                               DIV_ROUND_UP(w, BLOCKX * size_of_alignment_type),
                                               DIV_ROUND_UP(h, BLOCKY), 1, BLOCKX, BLOCKY, 1, 0,
                                               stream, args_vert, NULL));
    }
    {
        /*
         * ADR-0743: __launch_bounds__(128, 10) on filter1d_8_horizontal_kernel_2_17_9
         * reduced registers from 56 to 48.  vpt=2 is retained (vpt=4 evaluated and
         * rejected — smem-limited at 37.5% occupancy vs 62.5% for vpt=2).
         */
        const int BLOCKX = 128;
        const int BLOCKY = 1;
        const int val_per_thread = 2;

        void *args_hori[] = {
            &*buf, &w, &h, (uint16_t *)&vif_filter1d_table, &vif_enhn_gain_limit, &buf->accum};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_filter1d_8_horizontal_kernel_2_17_9,
                                               DIV_ROUND_UP(w, BLOCKX * val_per_thread),
                                               DIV_ROUND_UP(h, BLOCKY), 1, BLOCKX, BLOCKY, 1, 0,
                                               stream, args_hori, NULL));
    }
    return 0;
}

typedef struct VifFilter16Launch {
    int32_t add_shift_round_hp;
    int32_t shift_hp;
    int32_t add_shift_round_vp;
    int32_t shift_vp;
    int32_t add_shift_round_vp_sq;
    int32_t shift_vp_sq;
    int block_vert_x;
    int block_vert_y;
    int grid_vert_x;
    int grid_vert_y;
    int grid_hori_x;
    int grid_hori_y;
} VifFilter16Launch;

static VifFilter16Launch configure_filter1d_16(int w, int h, int scale, int bpc)
{
    VifFilter16Launch cfg = {
        .add_shift_round_hp = 32768,
        .shift_hp = 16,
        .add_shift_round_vp = scale == 0 ? 1 << (bpc - 1) : 32768,
        .shift_vp = scale == 0 ? bpc : 16,
        .add_shift_round_vp_sq = scale == 0 ? ((bpc == 8) ? 0 : 1 << ((bpc - 8) * 2 - 1)) : 32768,
        .shift_vp_sq = scale == 0 ? (bpc - 8) * 2 : 16,
    };
    const int values_per_thread = (int)(2 * sizeof(unsigned) / sizeof(uint16_t));
    cfg.block_vert_x = VMAF_CUDA_CACHE_LINE_SIZE / values_per_thread;
    cfg.block_vert_y = 128 / cfg.block_vert_x;
    cfg.grid_vert_x = DIV_ROUND_UP(w, cfg.block_vert_x * values_per_thread);
    cfg.grid_vert_y = DIV_ROUND_UP(h, cfg.block_vert_y);
    cfg.grid_hori_x = DIV_ROUND_UP(w, 128);
    cfg.grid_hori_y = h;
    return cfg;
}

static int select_filter1d_16_kernels(VifStateCuda *s, int scale, CUfunction *vertical,
                                      CUfunction *horizontal)
{
    const CUfunction vertical_functions[] = {
        s->func_filter1d_16_vertical_kernel_uint2_17_9_0,
        s->func_filter1d_16_vertical_kernel_uint2_9_5_1,
        s->func_filter1d_16_vertical_kernel_uint2_5_3_2,
        s->func_filter1d_16_vertical_kernel_uint2_3_0_3,
    };
    const CUfunction horizontal_functions[] = {
        s->func_filter1d_16_horizontal_kernel_2_17_9_0,
        s->func_filter1d_16_horizontal_kernel_2_9_5_1,
        s->func_filter1d_16_horizontal_kernel_2_5_3_2,
        s->func_filter1d_16_horizontal_kernel_2_3_0_3,
    };
    if (scale < 0 || scale >= 4) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_vif_cuda: invalid VIF scale %d\n", scale);
        return -EINVAL;
    }
    *vertical = vertical_functions[scale];
    *horizontal = horizontal_functions[scale];
    return 0;
}

static int filter1d_16(VifStateCuda *s, VifBufferCuda *buf, uint16_t *ref_in, uint16_t *dis_in,
                       int w, int h, int scale, int bpc, double vif_enhn_gain_limit,
                       CudaFunctions *cu_f, CUstream stream)
{
    VifFilter16Launch cfg = configure_filter1d_16(w, h, scale, bpc);
    CUfunction vertical = NULL;
    CUfunction horizontal = NULL;
    int err = select_filter1d_16_kernels(s, scale, &vertical, &horizontal);
    if (err)
        return err;

    void *args_vert[] = {&*buf,
                         &ref_in,
                         &dis_in,
                         &w,
                         &h,
                         &cfg.add_shift_round_vp,
                         &cfg.shift_vp,
                         &cfg.add_shift_round_vp_sq,
                         &cfg.shift_vp_sq,
                         &(*(filter_table_stuct *)vif_filter1d_table)};
    vif_accums *accum = &((vif_accums *)buf->accum)[scale];
    void *args_hori[] = {&*buf,
                         &w,
                         &h,
                         &cfg.add_shift_round_hp,
                         &cfg.shift_hp,
                         (uint16_t *)&vif_filter1d_table,
                         &vif_enhn_gain_limit,
                         &accum};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(vertical, cfg.grid_vert_x, cfg.grid_vert_y, 1,
                                           cfg.block_vert_x, cfg.block_vert_y, 1, 0, stream,
                                           args_vert, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(horizontal, cfg.grid_hori_x, cfg.grid_hori_y, 1, 128, 1,
                                           1, 0, stream, args_hori, NULL));
    return 0;
}

typedef struct VifScore {
    struct {
        float num;
        float den;
    } scale[4];
} VifScore;

static VifScore calculate_vif_scores(const vif_accums *accum)
{
    VifScore vif = {0};
    for (unsigned scale = 0; scale < 4; ++scale) {
        vif.scale[scale].num =
            accum[scale].num_log / 2048.0 + accum[scale].x2 +
            (accum[scale].den_non_log - ((accum[scale].num_non_log) / 16384.0) / (65025.0));
        vif.scale[scale].den = accum[scale].den_log / 2048.0 -
                               (accum[scale].x + (accum[scale].num_x * 17)) +
                               accum[scale].den_non_log;
    }
    return vif;
}

static int append_vif_scale_scores(const write_score_parameters_vif *data, const VifScore *vif)
{
    static const char *const names[] = {
        "VMAF_integer_feature_vif_scale0_score",
        "VMAF_integer_feature_vif_scale1_score",
        "VMAF_integer_feature_vif_scale2_score",
        "VMAF_integer_feature_vif_scale3_score",
    };
    VifStateCuda *s = data->s;
    int err = 0;
    for (unsigned scale = 0; scale < 4; scale++) {
        const double score = (scale == 0 && s->vif_skip_scale0) ?
                                 0.0 :
                                 vif->scale[scale].num / vif->scale[scale].den;
        err |= vmaf_feature_collector_append_with_dict(
            data->feature_collector, s->feature_name_dict, names[scale], score, data->index);
    }
    return err;
}

static int append_vif_debug_scales(const write_score_parameters_vif *data, const VifScore *vif)
{
    static const char *const num_names[] = {"integer_vif_num_scale0", "integer_vif_num_scale1",
                                            "integer_vif_num_scale2", "integer_vif_num_scale3"};
    static const char *const den_names[] = {"integer_vif_den_scale0", "integer_vif_den_scale1",
                                            "integer_vif_den_scale2", "integer_vif_den_scale3"};
    VifStateCuda *s = data->s;
    int err = 0;
    if (s->vif_skip_scale0) {
        err |= vmaf_feature_collector_append_with_dict(
            data->feature_collector, s->feature_name_dict, num_names[0], 0.0, data->index);
        err |= vmaf_feature_collector_append_with_dict(
            data->feature_collector, s->feature_name_dict, den_names[0], -1.0, data->index);
    } else {
        err |=
            vmaf_feature_collector_append_with_dict(data->feature_collector, s->feature_name_dict,
                                                    num_names[0], vif->scale[0].num, data->index);
        err |=
            vmaf_feature_collector_append_with_dict(data->feature_collector, s->feature_name_dict,
                                                    den_names[0], vif->scale[0].den, data->index);
    }
    for (unsigned scale = 1; scale < 4; scale++) {
        err |= vmaf_feature_collector_append_with_dict(data->feature_collector,
                                                       s->feature_name_dict, num_names[scale],
                                                       vif->scale[scale].num, data->index);
        err |= vmaf_feature_collector_append_with_dict(data->feature_collector,
                                                       s->feature_name_dict, den_names[scale],
                                                       vif->scale[scale].den, data->index);
    }
    return err;
}

static int append_vif_debug_scores(const write_score_parameters_vif *data, const VifScore *vif)
{
    VifStateCuda *s = data->s;
    const double score_num =
        s->vif_skip_scale0 ?
            (double)vif->scale[1].num + (double)vif->scale[2].num + (double)vif->scale[3].num :
            (double)vif->scale[0].num + (double)vif->scale[1].num + (double)vif->scale[2].num +
                (double)vif->scale[3].num;
    const double score_den =
        s->vif_skip_scale0 ?
            (double)vif->scale[1].den + (double)vif->scale[2].den + (double)vif->scale[3].den :
            (double)vif->scale[0].den + (double)vif->scale[1].den + (double)vif->scale[2].den +
                (double)vif->scale[3].den;
    const double score = score_den == 0.0 ? 1.0f : score_num / score_den;
    int err = vmaf_feature_collector_append_with_dict(data->feature_collector, s->feature_name_dict,
                                                      "integer_vif", score, data->index);
    err |= vmaf_feature_collector_append_with_dict(data->feature_collector, s->feature_name_dict,
                                                   "integer_vif_num", score_num, data->index);
    err |= vmaf_feature_collector_append_with_dict(data->feature_collector, s->feature_name_dict,
                                                   "integer_vif_den", score_den, data->index);
    err |= append_vif_debug_scales(data, vif);
    return err;
}

static int write_scores(write_score_parameters_vif *data)
{
    const VifScore vif = calculate_vif_scores((const vif_accums *)data->s->buf.accum_host);
    int err = append_vif_scale_scores(data, &vif);
    if (data->s->debug)
        err |= append_vif_debug_scores(data, &vif);
    return err;
}

static int run_vif_scale(VifStateCuda *s, VmafPicture *ref_pic, VmafPicture *dist_pic, int width,
                         int height, unsigned scale, CudaFunctions *cu_f, CUstream picture_stream)
{
    if (scale == 0 && ref_pic->bpc == 8) {
        return filter1d_8(s, &s->buf, ref_pic->data[0], dist_pic->data[0], width, height,
                          s->vif_enhn_gain_limit, cu_f, picture_stream);
    }
    uint16_t *ref = ref_pic->data[0];
    uint16_t *dist = dist_pic->data[0];
    CUstream stream = picture_stream;
    if (scale > 0) {
        store_vif_device_pointer(&ref, s->buf.ref);
        store_vif_device_pointer(&dist, s->buf.dis);
        stream = s->str;
    }
    return filter1d_16(s, &s->buf, ref, dist, width, height, (int)scale, (int)ref_pic->bpc,
                       s->vif_enhn_gain_limit, cu_f, stream);
}

static int run_vif_scales(VifStateCuda *s, VmafPicture *ref_pic, VmafPicture *dist_pic,
                          CudaFunctions *cu_f, CUstream picture_stream)
{
    int width = (int)ref_pic->w[0];
    int height = (int)dist_pic->h[0];
    for (unsigned scale = 0; scale < 4; scale++) {
        if (scale > 0) {
            width /= 2;
            height /= 2;
        }
        const int err =
            run_vif_scale(s, ref_pic, dist_pic, width, height, scale, cu_f, picture_stream);
        if (err)
            return err;
        if (scale == 0) {
            CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->event, picture_stream));
            CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->event, CU_EVENT_WAIT_DEFAULT));
        }
    }
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)index;
    (void)ref_pic_90;
    (void)dist_pic_90;
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    if (s->n_planes != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_vif_cuda: expected one luma plane, got %u\n",
                 s->n_planes);
        return -EINVAL;
    }

    const CUstream picture_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f,
                      cuMemsetD8Async(s->buf.accum_data->data, 0, sizeof(vif_accums) * 4, s->str));
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(picture_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));
    int err = run_vif_scales(s, ref_pic, dist_pic, cu_f, picture_stream);
    if (err)
        return err;
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->buf.accum_host, s->buf.accum_data->data,
                                              sizeof(vif_accums) * 4, s->str));
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->finished, s->str));
    err = vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    if (err == -ENOSPC) {
        s->drained = false;
        return 0;
    }
    if (err)
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_vif_cuda: drain registration failed: %d\n", err);
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    if (s->drained) {
        s->drained = false;
    } else {
        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    }

    // Results are now in accum_host — compute and write scores
    write_score_parameters_vif data = {
        .feature_collector = feature_collector,
        .s = s,
        .index = index,
    };
    return write_scores(&data);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    return release_vif_resources(fex, true);
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    VifStateCuda *s = fex->priv;

    CHECK_CUDA_RETURN(fex->cu_state->f, cuStreamSynchronize(s->str));
    return 1;
}

static const char *provided_features[] = {"VMAF_integer_feature_vif_scale0_score",
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
                                          NULL};

VmafFeatureExtractor vmaf_fex_integer_vif_cuda = {.name = "vif_cuda",
                                                  .init = init_fex_cuda,
                                                  .submit = submit_fex_cuda,
                                                  .collect = collect_fex_cuda,
                                                  .flush = flush_fex_cuda,
                                                  .options = options,
                                                  .close = close_fex_cuda,
                                                  .priv_size = sizeof(VifStateCuda),
                                                  .provided_features = provided_features,
                                                  .flags = VMAF_FEATURE_EXTRACTOR_CUDA};

/* NOLINTEND(modernize-use-nullptr) */
