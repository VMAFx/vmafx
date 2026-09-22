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

/* ------------------------------------------------------------------ */
/* vif_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: lifted verbatim from the former `free_ref` label. The same
 * resources are released in the same order on every exit path, and the
 * value returned is the one the label returned.
 */
static int vif_init_unwind(VmafFeatureExtractor *fex, VifStateCuda *s, CudaFunctions *cu_f, int ret)
{
    if (s->buf.data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.data);
        free(s->buf.data);
    }
    if (s->buf.accum_data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.accum_data);
        free(s->buf.accum_data);
    }
    if (s->buf.accum_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.accum_host);
    }
    (void)ret; // accumulated cleanup status intentionally discarded on error path

    /* The context is already popped (cuCtxPopCurrent at the end of module
     * setup) before any `free_ref` jump fires, so we must not route through
     * the `fail` label (it would double-pop).  Tear down the module, events,
     * and stream explicitly here, mirroring the `fail_after_module` ladder. */
    (void)cu_f->cuModuleUnload(s->filter1d_module);
    s->filter1d_module = NULL;
    (void)cu_f->cuEventDestroy(s->finished);
    s->finished = 0;
    (void)cu_f->cuEventDestroy(s->event);
    s->event = 0;
    (void)cu_f->cuStreamDestroy(s->str);
    s->str = 0;

    return -ENOMEM;
}

/* VifInitStage - how far init_fex_cuda's CUDA-context setup got before it
 * failed, i.e. which rung of the former `fail_after_*` label ladder the
 * unwind has to start at. */
typedef enum VifInitStage {
    VIF_INIT_POP_ONLY = 0,
    VIF_INIT_AFTER_STREAM,
    VIF_INIT_AFTER_EVENT,
    VIF_INIT_AFTER_FINISHED,
    VIF_INIT_AFTER_MODULE,
} VifInitStage;

/* vif_init_kernel_unwind - the former graduated fail_after_* ladder.
 *
 * HISS-01 / HISS-04: the ladder's statements are lifted verbatim and still
 * run in the same fall-through order, so every exit path releases the same
 * resources in the same sequence and returns the same errno.
 */
static int vif_init_kernel_unwind(VifStateCuda *s, CudaFunctions *cu_f, VifInitStage stage,
                                  int cuda_err)
{
    if (stage >= VIF_INIT_AFTER_MODULE) {
        /* cuModuleGetFunction failed — unload the module before releasing stream
         * and events.  cuModuleUnload is safe even if no kernels were resolved. */
        (void)cu_f->cuModuleUnload(s->filter1d_module);
        s->filter1d_module = NULL;
    }
    if (stage >= VIF_INIT_AFTER_FINISHED) {
        (void)cu_f->cuEventDestroy(s->finished);
        s->finished = 0;
    }
    if (stage >= VIF_INIT_AFTER_EVENT) {
        (void)cu_f->cuEventDestroy(s->event);
        s->event = 0;
    }
    if (stage >= VIF_INIT_AFTER_STREAM) {
        (void)cu_f->cuStreamDestroy(s->str);
        s->str = 0;
    }
    (void)cu_f->cuCtxPopCurrent(NULL);
    return cuda_err;
}

/* vif_create_stream_and_events - stream, the two events and the PTX module.
 *
 * `*stage` tracks which rung the unwind must start at, exactly matching the
 * label each CHECK_CUDA_GOTO used to jump to.
 */
static int vif_create_stream_and_events(VifStateCuda *s, CudaFunctions *cu_f, VifInitStage *stage)
{
    *stage = VIF_INIT_POP_ONLY;
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0));
    /* ADR-1090 — graduated stages so each earlier allocation is freed when a
     * later one fails; previously all paths only popped the context, leaking
     * the stream and any events already created. */
    *stage = VIF_INIT_AFTER_STREAM;
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->event, CU_EVENT_DEFAULT));
    *stage = VIF_INIT_AFTER_EVENT;
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT));
    *stage = VIF_INIT_AFTER_FINISHED;
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->filter1d_module, filter1d_ptx));
    *stage = VIF_INIT_AFTER_MODULE;
    return 0;
}

/* vif_get_filter1d_functions - the ten filter1d kernel handles. */
static int vif_get_filter1d_functions(VifStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_8_vertical_kernel_uint32_t_17_9,
                                                s->filter1d_module,
                                                "filter1d_8_vertical_kernel_uint32_t_17_9"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_8_horizontal_kernel_2_17_9,
                                                s->filter1d_module,
                                                "filter1d_8_horizontal_kernel_2_17_9"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_vertical_kernel_uint2_17_9_0,
                                                s->filter1d_module,
                                                "filter1d_16_vertical_kernel_uint2_17_9_0"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_vertical_kernel_uint2_9_5_1,
                                                s->filter1d_module,
                                                "filter1d_16_vertical_kernel_uint2_9_5_1"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_vertical_kernel_uint2_5_3_2,
                                                s->filter1d_module,
                                                "filter1d_16_vertical_kernel_uint2_5_3_2"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_vertical_kernel_uint2_3_0_3,
                                                s->filter1d_module,
                                                "filter1d_16_vertical_kernel_uint2_3_0_3"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_horizontal_kernel_2_17_9_0,
                                                s->filter1d_module,
                                                "filter1d_16_horizontal_kernel_2_17_9_0"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_horizontal_kernel_2_9_5_1,
                                                s->filter1d_module,
                                                "filter1d_16_horizontal_kernel_2_9_5_1"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_horizontal_kernel_2_5_3_2,
                                                s->filter1d_module,
                                                "filter1d_16_horizontal_kernel_2_5_3_2"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_filter1d_16_horizontal_kernel_2_3_0_3,
                                                s->filter1d_module,
                                                "filter1d_16_horizontal_kernel_2_3_0_3"));
    return 0;
}

/* vif_init_cuda_context - push the context, create stream, events and module,
 * resolve every kernel handle and pop the context again.
 *
 * HISS-01 / HISS-04: lifted out of init_fex_cuda. The former `fail` and
 * `fail_after_pop` labels are the two CHECK_CUDA_RETURN sites here (a failed
 * push pops nothing, a failed pop returns straight away without releasing
 * stream, events or module — both exactly as before); every other failure
 * routes through vif_init_kernel_unwind() at the same rung.
 */
static int vif_init_cuda_context(VmafFeatureExtractor *fex, VifStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));

    VifInitStage stage = VIF_INIT_POP_ONLY;
    int err = vif_create_stream_and_events(s, cu_f, &stage);
    if (err)
        return vif_init_kernel_unwind(s, cu_f, stage, err);

    err = vif_get_filter1d_functions(s, cu_f);
    if (err)
        return vif_init_kernel_unwind(s, cu_f, VIF_INIT_AFTER_MODULE, err);

    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return 0;
}

/* vif_carve_buffers - carve the flat device allocation into typed regions.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; the carve order decides
 * every region's device address, so it is unchanged.
 *
 * CUDA device pointers arrive as CUdeviceptr (unsigned long long) and must
 * be cast to host-visible pointer types to carve the buffer into typed
 * subregions the launch-time kernel-args struct references. The cast is
 * inherent to the CUDA Driver API and cannot be refactored away without
 * changing the public libvmaf-CUDA contract. Per the touched-file
 * rule, upstream-parity exception (ADR-0141 §2 load-bearing invariant). */
// NOLINTBEGIN(performance-no-int-to-ptr)
static int vif_carve_buffers(VmafFeatureExtractor *fex, VifStateCuda *s, CudaFunctions *cu_f,
                             unsigned h, size_t rd_size)
{
    CUdeviceptr data;
    int ret = vmaf_cuda_buffer_get_dptr(s->buf.data, &data);
    if (ret)
        return vif_init_unwind(fex, s, cu_f, ret);

    s->buf.ref = data;
    data += rd_size;
    s->buf.dis = data;
    data += rd_size;
    s->buf.mu1 = (uint16_t *)data;
    data += h * s->buf.stride_16;
    s->buf.mu2 = (uint16_t *)data;
    data += h * s->buf.stride_16;
    s->buf.mu1_32 = (uint32_t *)data;
    data += h * s->buf.stride_32;
    s->buf.mu2_32 = (uint32_t *)data;
    data += h * s->buf.stride_32;
    s->buf.ref_sq = (uint32_t *)data;
    data += h * s->buf.stride_32;
    s->buf.dis_sq = (uint32_t *)data;
    data += h * s->buf.stride_32;
    s->buf.ref_dis = (uint32_t *)data;
    data += h * s->buf.stride_32;
    s->buf.tmp.mu1 = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.mu2 = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.ref = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.dis = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.ref_dis = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.ref_convol = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.dis_convol = (uint32_t *)data;
    data += s->buf.stride_tmp * h;
    s->buf.tmp.padding = (uint32_t *)data;
    data += s->buf.stride_tmp * h;

    CUdeviceptr data_accum;
    ret = vmaf_cuda_buffer_get_dptr(s->buf.accum_data, &data_accum);
    if (ret)
        return vif_init_unwind(fex, s, cu_f, ret);

    s->buf.accum = (int64_t *)data_accum;
    return 0;
}
// NOLINTEND(performance-no-int-to-ptr)

/* vif_setup_buffers - the stride arithmetic plus every allocation.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; the stride expressions and
 * the allocation order are unchanged, and each failure still unwinds through
 * vif_init_unwind().
 */
static int vif_setup_buffers(VmafFeatureExtractor *fex, VifStateCuda *s, CudaFunctions *cu_f,
                             unsigned w, unsigned h, int tex_alignment, bool hbd)
{
    s->buf.stride = tex_alignment * (((w * (1 << (int)hbd) + tex_alignment - 1) / tex_alignment));
    {
        const int rd_w_bytes = ((w + 1) / 2) * sizeof(uint16_t);
        s->buf.rd_stride = tex_alignment * ((rd_w_bytes + tex_alignment - 1) / tex_alignment);
    }
    s->buf.stride_16 = ALIGN_CEIL(w * sizeof(uint16_t));
    s->buf.stride_32 = ALIGN_CEIL(w * sizeof(uint32_t));
    s->buf.stride_64 = ALIGN_CEIL(w * sizeof(uint64_t));
    s->buf.stride_tmp = ALIGN_CEIL(w * sizeof(uint32_t));
    const size_t rd_size = s->buf.rd_stride * ((h + 1) / 2);
    const size_t data_sz = 2 * rd_size + 2 * (h * s->buf.stride_16) + 5 * (h * s->buf.stride_32) +
                           8 * (s->buf.stride_tmp * h); // intermediater buffers
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.data, data_sz);
    if (ret)
        return vif_init_unwind(fex, s, cu_f, ret);

    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.accum_data, sizeof(vif_accums) * 4);
    if (ret)
        return vif_init_unwind(fex, s, cu_f, ret);

    ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->buf.accum_host,
                                      sizeof(vif_accums) * 4);
    if (ret)
        return vif_init_unwind(fex, s, cu_f, ret);

    return vif_carve_buffers(fex, s, cu_f, h, rd_size);
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    const int cuda_err = vif_init_cuda_context(fex, s, cu_f);
    if (cuda_err)
        return cuda_err;

    /* VIF is luma-only by design across every backend (CPU, CUDA, HIP, SYCL,
     * Vulkan, Metal) and across upstream Netflix/vmaf — the metric (Sheikh &
     * Bovik, 2006) is defined on a single luminance channel.  `n_planes` is
     * hardcoded to 1 to match the CPU twin (`libvmaf/src/feature/integer_vif.c`,
     * which reads `data[0]` only and has no `enable_chroma` option).  The
     * `enable_chroma` option above is vestigial — retained so callers passing
     * it on the CLI / in model JSONs do not see an option-not-recognised
     * error — and warn-on-true here surfaces the no-op behaviour instead of
     * silently producing luma-only output that contradicts the request.
     * See ADR-0541. */
    if (s->enable_chroma) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_vif (CUDA): enable_chroma=true requested but VIF is "
                 "luma-only by design (matches CPU integer_vif and upstream "
                 "Netflix/vmaf); option is a no-op. See ADR-0541.\n");
        s->enable_chroma = false;
    }
    (void)pix_fmt; /* YUV400P needs no special case — luma-only path handles it. */
    s->n_planes = 1;

    const bool hbd = bpc > 8;

    int tex_alignment;
    CHECK_CUDA_RETURN(cu_f,
                      cuDeviceGetAttribute(&tex_alignment, CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT,
                                           fex->cu_state->dev));

    int ret = vif_setup_buffers(fex, s, cu_f, w, h, tex_alignment, hbd);
    if (ret)
        return ret;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return vif_init_unwind(fex, s, cu_f, ret);
    return 0;
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

/* VifShift16 - the six rounding shifts filter1d_16 hands to its kernels. */
typedef struct VifShift16 {
    int32_t add_shift_round_HP;
    int32_t shift_HP;
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int32_t add_shift_round_VP_sq;
    int32_t shift_VP_sq;
} VifShift16;

/* filter1d_16_shifts - the per-scale rounding constants.
 *
 * HISS-04: lifted verbatim out of filter1d_16; both branches assign exactly
 * the values they did before.
 */
static void filter1d_16_shifts(int scale, int bpc, VifShift16 *sh)
{
    if (scale == 0) {
        sh->shift_HP = 16;
        sh->add_shift_round_HP = 32768;
        sh->shift_VP = bpc;
        sh->add_shift_round_VP = 1 << (bpc - 1);
        sh->shift_VP_sq = (bpc - 8) * 2;
        sh->add_shift_round_VP_sq = (bpc == 8) ? 0 : 1 << (sh->shift_VP_sq - 1);
    } else {
        sh->shift_HP = 16;
        sh->add_shift_round_HP = 32768;
        sh->shift_VP = 16;
        sh->add_shift_round_VP = 32768;
        sh->shift_VP_sq = 16;
        sh->add_shift_round_VP_sq = 32768;
    }
}

/* VifGrid16 - filter1d_16's launch geometry. */
typedef struct VifGrid16 {
    int blockx;
    int block_vert_x;
    int block_vert_y;
    int grid_vert_x;
    int grid_vert_y;
    int grid_hori_x;
    int grid_hori_y;
} VifGrid16;

/* filter1d_16_grid - the launch geometry.
 *
 * HISS-04: lifted verbatim out of filter1d_16; every expression is the one
 * the identically named local used.
 */
static void filter1d_16_grid(int w, int h, VifGrid16 *g)
{
    struct uint2 {
        unsigned x, y;
    } uint2;

    const int size_of_alginment = sizeof(uint2);
    const int val_per_thread = size_of_alginment / sizeof(uint16_t);
    g->blockx = 128;
    g->block_vert_x = VMAF_CUDA_CACHE_LINE_SIZE / val_per_thread;
    g->block_vert_y = 128 / (VMAF_CUDA_CACHE_LINE_SIZE / val_per_thread);
    g->grid_vert_x = DIV_ROUND_UP(w, g->block_vert_x * val_per_thread);
    g->grid_vert_y = DIV_ROUND_UP(h, g->block_vert_y);
    g->grid_hori_x = DIV_ROUND_UP(w, g->blockx);
    g->grid_hori_y = h;
}

static int filter1d_16(VifStateCuda *s, VifBufferCuda *buf, uint16_t *ref_in, uint16_t *dis_in,
                       int w, int h, int scale, int bpc, double vif_enhn_gain_limit,
                       CudaFunctions *cu_f, CUstream stream)
{
    VifShift16 sh;
    filter1d_16_shifts(scale, bpc, &sh);

    VifGrid16 g;
    filter1d_16_grid(w, h, &g);

    void *args_vert[] = {&*buf,
                         &ref_in,
                         &dis_in,
                         &w,
                         &h,
                         &sh.add_shift_round_VP,
                         &sh.shift_VP,
                         &sh.add_shift_round_VP_sq,
                         &sh.shift_VP_sq,
                         &(*(filter_table_stuct *)vif_filter1d_table)};

    vif_accums *ptr = &((vif_accums *)buf->accum)[scale];

    void *args_hori[] = {&*buf,
                         &w,
                         &h,
                         &sh.add_shift_round_HP,
                         &sh.shift_HP,
                         (uint16_t *)&vif_filter1d_table,
                         &vif_enhn_gain_limit,
                         &(ptr)};

    CUfunction vert_kernel;
    CUfunction hori_kernel;
    switch (scale) {
    case 0:
        vert_kernel = s->func_filter1d_16_vertical_kernel_uint2_17_9_0;
        hori_kernel = s->func_filter1d_16_horizontal_kernel_2_17_9_0;
        break;
    case 1:
        vert_kernel = s->func_filter1d_16_vertical_kernel_uint2_9_5_1;
        hori_kernel = s->func_filter1d_16_horizontal_kernel_2_9_5_1;
        break;
    case 2:
        vert_kernel = s->func_filter1d_16_vertical_kernel_uint2_5_3_2;
        hori_kernel = s->func_filter1d_16_horizontal_kernel_2_5_3_2;
        break;
    case 3:
        vert_kernel = s->func_filter1d_16_vertical_kernel_uint2_3_0_3;
        hori_kernel = s->func_filter1d_16_horizontal_kernel_2_3_0_3;
        break;
    default:
        return 0; /* VIF has exactly 4 scales (0–3); unreachable with valid input */
    }

    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(vert_kernel, g.grid_vert_x, g.grid_vert_y, 1, g.block_vert_x,
                                     g.block_vert_y, 1, 0, stream, args_vert, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(hori_kernel, g.grid_hori_x, g.grid_hori_y, 1, g.blockx,
                                           1, 1, 0, stream, args_hori, NULL));
    return 0;
}

typedef struct VifScore {
    struct {
        float num;
        float den;
    } scale[4];
} VifScore;

/* vif_reduce_accums - fold the four per-scale accumulators into num/den.
 *
 * HISS-04: lifted verbatim out of write_scores. Each num and den stays a
 * single unchanged expression, so no FP contraction decision moves.
 */
static void vif_reduce_accums(const vif_accums *accum, VifScore *vif)
{
    for (unsigned scale = 0; scale < 4; ++scale) {
        vif->scale[scale].num =
            accum[scale].num_log / 2048.0 + accum[scale].x2 +
            (accum[scale].den_non_log - ((accum[scale].num_non_log) / 16384.0) / (65025.0));
        vif->scale[scale].den = accum[scale].den_log / 2048.0 -
                                (accum[scale].x + (accum[scale].num_x * 17)) +
                                accum[scale].den_non_log;
    }
}

/* vif_append_debug_scale_pairs - the per-scale num/den rows for scales 1-3.
 *
 * HISS-04: lifted verbatim out of write_scores; same order, same names.
 */
static int vif_append_debug_scale_pairs(VmafFeatureCollector *feature_collector, VifStateCuda *s,
                                        const VifScore *vif, unsigned index)
{
    int err = 0;
    err |=
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "integer_vif_num_scale1", vif->scale[1].num, index);

    err |=
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "integer_vif_den_scale1", vif->scale[1].den, index);

    err |=
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "integer_vif_num_scale2", vif->scale[2].num, index);

    err |=
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "integer_vif_den_scale2", vif->scale[2].den, index);

    err |=
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "integer_vif_num_scale3", vif->scale[3].num, index);

    err |=
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "integer_vif_den_scale3", vif->scale[3].den, index);

    return err;
}

/* vif_append_debug_scores - the debug-only rows.
 *
 * HISS-04: lifted verbatim out of write_scores. The totals keep their whole
 * expressions and the rows are appended in the same order; the caller folds
 * the returned status into its own with the same bitwise OR.
 */
static int vif_append_debug_scores(VmafFeatureCollector *feature_collector, VifStateCuda *s,
                                   const VifScore *vif, unsigned index)
{
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

    int err = 0;
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
                                                       "integer_vif_num_scale0", vif->scale[0].num,
                                                       index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_den_scale0", vif->scale[0].den,
                                                       index);
    }

    return err | vif_append_debug_scale_pairs(feature_collector, s, vif, index);
}

static int write_scores(write_score_parameters_vif *data)
{
    VmafFeatureCollector *feature_collector = data->feature_collector;
    VifStateCuda *s = data->s;
    unsigned index = data->index;

    VifScore vif;
    vif_reduce_accums((const vif_accums *)data->s->buf.accum_host, &vif);
    int err = 0;

    /* When vif_skip_scale0 is set, emit 0.0 for the finest scale (parity
     * with integer_vif.c CPU path, lines 698-706 and 747-761). The GPU
     * still computes scale 0 but we discard its contribution here. */
    if (s->vif_skip_scale0) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_vif_scale0_score", 0.0,
                                                       index);
    } else {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_vif_scale0_score",
                                                       vif.scale[0].num / vif.scale[0].den, index);
    }

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

    return err | vif_append_debug_scores(feature_collector, s, &vif, index);
}

/* vif_dispatch_filter - pick the filter1d variant this scale needs.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda's scale loop.
 */
static int vif_dispatch_filter(VifStateCuda *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                               VmafPicture *dist_pic, int w, int h, unsigned scale)
{
    if (ref_pic->bpc == 8 && scale == 0) {
        return filter1d_8(s, &s->buf, (uint8_t *)ref_pic->data[0], (uint8_t *)dist_pic->data[0], w,
                          h, s->vif_enhn_gain_limit, cu_f, vmaf_cuda_picture_get_stream(ref_pic));
    }
    if (scale == 0) {
        return filter1d_16(s, &s->buf, (uint16_t *)ref_pic->data[0], (uint16_t *)dist_pic->data[0],
                           w, h, scale, ref_pic->bpc, s->vif_enhn_gain_limit, cu_f,
                           vmaf_cuda_picture_get_stream(ref_pic));
    }
    // s->buf.ref / s->buf.dis are CUdeviceptr (unsigned long long) carved
    // from the master buffer in init; the filter1d_16 contract takes a
    // typed uint16_t* view. Cast is inherent to the CUDA Driver API.
    // Per ADR-0141 touched-file rule, upstream-parity exception.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return filter1d_16(s, &s->buf, (uint16_t *)s->buf.ref, (uint16_t *)s->buf.dis, w, h, scale,
                       ref_pic->bpc, s->vif_enhn_gain_limit, cu_f, s->str);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    (void)ref_pic_90;
    (void)dist_pic_90;
    /* n_planes is always 1: VIF is luma-only by design across every backend
     * (matches CPU integer_vif and upstream Netflix/vmaf — see ADR-0541).
     * The loop is retained for shape-parity with the CPU/HIP/SYCL twins. */
    for (unsigned plane = 0; plane < s->n_planes; ++plane) {
        int w = ref_pic->w[plane];
        int h = dist_pic->h[plane];

        CHECK_CUDA_RETURN(
            cu_f, cuMemsetD8Async(s->buf.accum_data->data, 0, sizeof(vif_accums) * 4, s->str));
        CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(vmaf_cuda_picture_get_stream(ref_pic),
                                                  vmaf_cuda_picture_get_ready_event(dist_pic),
                                                  CU_EVENT_WAIT_DEFAULT));
        for (unsigned scale = 0; scale < 4; ++scale) {
            if (scale > 0) {
                w /= 2;
                h /= 2;
            }

            const int err = vif_dispatch_filter(s, cu_f, ref_pic, dist_pic, w, h, scale);
            if (err)
                return err;
            if (scale == 0) {
                // This event ensures the input buffer is consumed
                CHECK_CUDA_RETURN(cu_f,
                                  cuEventRecord(s->event, vmaf_cuda_picture_get_stream(ref_pic)));
                CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->event, CU_EVENT_WAIT_DEFAULT));
            }
        }

        // Queue async download of accumulators
        CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->buf.accum_host, s->buf.accum_data->data,
                                                  sizeof(vif_accums) * 4, s->str));
        CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->finished, s->str));
        /* Engine-scope fence batching opt-in (T-GPU-OPT-1, ADR-0242). */
        (void)vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    }
    return 0;
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
    VifStateCuda *s = fex->priv;
    /* Close path continues unwinding on CUDA error. */
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(fex->cu_state->f, cuStreamSynchronize(s->str), after_sync);
after_sync:
    CHECK_CUDA_GOTO(fex->cu_state->f, cuStreamDestroy(s->str), after_stream);
after_stream:
    CHECK_CUDA_GOTO(fex->cu_state->f, cuEventDestroy(s->event), after_ev1);
after_ev1:
    CHECK_CUDA_GOTO(fex->cu_state->f, cuEventDestroy(s->finished), after_ev2);
after_ev2:;

    int ret = _cuda_err;
    if (s->buf.data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.data);
        free(s->buf.data);
    }
    if (s->buf.accum_data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.accum_data);
        free(s->buf.accum_data);
    }
    if (s->buf.accum_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.accum_host);
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f_close = fex->cu_state->f;
    if (cu_f_close && s->filter1d_module)
        (void)cu_f_close->cuModuleUnload(s->filter1d_module);
    return ret;
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
