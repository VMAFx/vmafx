/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cpu.h"
#include "common/macros.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "feature/nonfinite_score.h"

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
                                          * across every backend (CPU, CUDA, HIP, SYCL, Metal)
                                          * and across upstream Netflix/vmaf — see
                                          * ADR-0597.  Setting `enable_chroma=true` emits a
                                          * one-shot warning during init() and otherwise has
                                          * no effect on the produced scores; their names
                                          * carry `_enable_chroma` (ADR-1836). */
                                         .name = "enable_chroma",
                                         .help =
                                             "no-op (luma-only kernel; ADR-0597). retained for "
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
                                         .alias = "ssclz",
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
static int vif_init_unwind(VmafFeatureExtractor *fex, VifStateCuda *s, int ret)
{
    int rc = ret;
    int phase_rc = vmaf_cuda_stream_destroy(fex->cu_state, &s->str, true);
    if (phase_rc)
        return rc ? rc : phase_rc;

    phase_rc = vmaf_cuda_event_destroy(fex->cu_state, &s->finished);
    const int event_rc = vmaf_cuda_event_destroy(fex->cu_state, &s->event);
    if (!phase_rc)
        phase_rc = event_rc;
    if (phase_rc)
        return rc ? rc : phase_rc;

    int e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->buf.data);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->buf.accum_data);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->buf.accum_host);
    if (e && !rc)
        rc = e;
    e = vmaf_dictionary_free(&s->feature_name_dict);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_module_unload(fex->cu_state, &s->filter1d_module);
    if (e && !rc)
        rc = e;
    return rc;
}

/* Release every context-owned handle created before kernel setup failed. */
static int vif_init_kernel_unwind(VmafFeatureExtractor *fex, VifStateCuda *s, CudaFunctions *cu_f,
                                  int cuda_err)
{
    (void)cu_f->cuCtxPopCurrent(NULL);
    return vif_init_unwind(fex, s, cuda_err);
}

/* vif_create_stream_and_events - stream, the two events and the PTX module. */
static int vif_create_stream_and_events(VifStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0));
    /* ADR-1090 — graduated stages so each earlier allocation is freed when a
     * later one fails; previously all paths only popped the context, leaking
     * the stream and any events already created. */
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->event, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->filter1d_module, filter1d_ptx));
    return 0;
}

/* vif_get_filter1d_functions - the ten filter1d kernel handles, resolved in
 * table order; the first failure is returned. */
static int vif_get_filter1d_functions(VifStateCuda *s, CudaFunctions *cu_f)
{
    const struct {
        CUfunction *fn;
        const char *name;
    } kernels[] = {
        {&s->func_filter1d_8_vertical_kernel_uint32_t_17_9,
         "filter1d_8_vertical_kernel_uint32_t_17_9"},
        {&s->func_filter1d_8_horizontal_kernel_2_17_9, "filter1d_8_horizontal_kernel_2_17_9"},
        {&s->func_filter1d_16_vertical_kernel_uint2_17_9_0,
         "filter1d_16_vertical_kernel_uint2_17_9_0"},
        {&s->func_filter1d_16_vertical_kernel_uint2_9_5_1,
         "filter1d_16_vertical_kernel_uint2_9_5_1"},
        {&s->func_filter1d_16_vertical_kernel_uint2_5_3_2,
         "filter1d_16_vertical_kernel_uint2_5_3_2"},
        {&s->func_filter1d_16_vertical_kernel_uint2_3_0_3,
         "filter1d_16_vertical_kernel_uint2_3_0_3"},
        {&s->func_filter1d_16_horizontal_kernel_2_17_9_0, "filter1d_16_horizontal_kernel_2_17_9_0"},
        {&s->func_filter1d_16_horizontal_kernel_2_9_5_1, "filter1d_16_horizontal_kernel_2_9_5_1"},
        {&s->func_filter1d_16_horizontal_kernel_2_5_3_2, "filter1d_16_horizontal_kernel_2_5_3_2"},
        {&s->func_filter1d_16_horizontal_kernel_2_3_0_3, "filter1d_16_horizontal_kernel_2_3_0_3"},
    };
    for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
        CHECK_CUDA_RETURN(cu_f,
                          cuModuleGetFunction(kernels[i].fn, s->filter1d_module, kernels[i].name));
    }
    return 0;
}

#define VIF_LOG2_TRANSFER_BLOCK 256u

int vmaf_cuda_vif_log2_table_transfer(VmafCudaState *cu_state, CUmodule module,
                                      VmafCudaBuffer *staging, bool to_module)
{
    CudaFunctions *cu_f = cu_state->f;
    CUfunction transfer = NULL;
    unsigned direction = to_module ? 1u : 0u;
    void *params[] = {(void *)staging, &direction};
    const unsigned grid =
        (VIF_LOG2_TABLE_SIZE + VIF_LOG2_TRANSFER_BLOCK - 1u) / VIF_LOG2_TRANSFER_BLOCK;

    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(cu_state->ctx));
    CUresult res = cu_f->cuModuleGetFunction(&transfer, module, "vif_cuda_log2_table_transfer");
    if (res == CUDA_SUCCESS) {
        res = cu_f->cuLaunchKernel(transfer, grid, 1, 1, VIF_LOG2_TRANSFER_BLOCK, 1, 1, 0,
                                   cu_state->str, params, NULL);
    }
    if (res == CUDA_SUCCESS)
        res = cu_f->cuStreamSynchronize(cu_state->str);
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
}

int vmaf_cuda_vif_upload_log2_table(VmafCudaState *cu_state, CUmodule module)
{
    const size_t table_bytes = VIF_LOG2_TABLE_SIZE * sizeof(uint16_t);
    uint16_t *table = malloc(table_bytes);
    if (!table)
        return -ENOMEM;
    vif_log2_table_generate(table);

    VmafCudaBuffer *staging = NULL;
    int err = vmaf_cuda_buffer_alloc(cu_state, &staging, table_bytes);
    if (!err)
        err = vmaf_cuda_buffer_upload_async(cu_state, staging, table, 0);
    /* The transfer waits for the stream, so the copy from `table` and the
     * kernel are both done before either buffer is released. */
    if (!err)
        err = vmaf_cuda_vif_log2_table_transfer(cu_state, module, staging, true);
    const int sync_err = vmaf_cuda_sync(cu_state);
    const int free_err = vmaf_cuda_buffer_free_owned(cu_state, &staging);
    free(table);
    if (err)
        return err;
    return sync_err ? sync_err : free_err;
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

    int err = vif_create_stream_and_events(s, cu_f);
    if (err)
        return vif_init_kernel_unwind(fex, s, cu_f, err);

    err = vif_get_filter1d_functions(s, cu_f);
    if (err)
        return vif_init_kernel_unwind(fex, s, cu_f, err);

    const CUresult pop_res = cu_f->cuCtxPopCurrent(NULL);
    if (pop_res != CUDA_SUCCESS)
        return vif_init_kernel_unwind(fex, s, cu_f, vmaf_cuda_result_to_errno((int)pop_res));
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
static int vif_carve_buffers(VmafFeatureExtractor *fex, VifStateCuda *s, unsigned h, size_t rd_size)
{
    CUdeviceptr data;
    int ret = vmaf_cuda_buffer_get_dptr(s->buf.data, &data);
    if (ret)
        return vif_init_unwind(fex, s, ret);

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
        return vif_init_unwind(fex, s, ret);

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
static int vif_setup_buffers(VmafFeatureExtractor *fex, VifStateCuda *s, unsigned w, unsigned h,
                             int tex_alignment, bool hbd)
{
    /* The picture pitches are set per frame (vif_submit_scales()); this is the
     * pitch of a picture the engine's pool allocates, until the first frame. */
    s->buf.stride =
        (ptrdiff_t)tex_alignment *
        ((w * (1u << (unsigned)hbd) + (unsigned)tex_alignment - 1u) / (unsigned)tex_alignment);
    s->buf.dis_stride = s->buf.stride;
    {
        const int rd_w_bytes = (int)(((w + 1u) / 2u) * sizeof(uint16_t));
        s->buf.rd_stride =
            (ptrdiff_t)tex_alignment * ((rd_w_bytes + tex_alignment - 1) / tex_alignment);
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
        return vif_init_unwind(fex, s, ret);

    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.accum_data, sizeof(vif_accums) * 4);
    if (ret)
        return vif_init_unwind(fex, s, ret);

    ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->buf.accum_host,
                                      sizeof(vif_accums) * 4);
    if (ret)
        return vif_init_unwind(fex, s, ret);

    return vif_carve_buffers(fex, s, h, rd_size);
}

/* Smallest frame dimension every scale filters like the CPU
 * (T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29, ADR-1374). Scale s works on
 * floor(dim / 2^s) samples and reflects each tap once; a tap half-width of k
 * stays in the plane only while that is >= k + 1, i.e. dim >= (k + 1) << s.
 * The scale filters {17, 9, 5, 3} need {9, 10, 12, 16} and the decimation
 * filters (the next scale's) {5, 6, 8}, so the bound is 16, the one vif_sycl
 * declares. Below it filter1d.cu clamps the taps a second reflection would
 * need, which keeps its loads in bounds but is not the CPU's value. */
static unsigned vif_cuda_min_dim(void)
{
    unsigned min_dim = 1u;
    for (unsigned scale = 0u; scale < 4u; scale++) {
        const unsigned need = (((unsigned)vif_filter1d_width[scale] / 2u) + 1u) << scale;
        const unsigned rd_width = (scale < 3u) ? (unsigned)vif_filter1d_width[scale + 1u] : 0u;
        const unsigned rd_need = rd_width ? ((rd_width / 2u) + 1u) << scale : 1u;
        min_dim = (need > min_dim) ? need : min_dim;
        min_dim = (rd_need > min_dim) ? rd_need : min_dim;
    }
    return min_dim;
}

/* ADR-1324 first-picture gate: model dispatch computes frames below the
 * minimum with the CPU `vif` extractor instead of this twin. */
static int check_context_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                              unsigned w, unsigned h)
{
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    const unsigned min_dim = vif_cuda_min_dim();
    return (w < min_dim || h < min_dim) ? -ENOTSUP : 0;
}

/* vif_drop_vestigial_chroma_option - warn about and clear `enable_chroma`.
 *
 * VIF is luma-only by design across every backend (CPU, CUDA, HIP, SYCL,
 * Vulkan, Metal) and across upstream Netflix/vmaf — the metric (Sheikh &
 * Bovik, 2006) is defined on a single luminance channel.  `n_planes` is
 * hardcoded to 1 to match the CPU twin (`libvmaf/src/feature/integer_vif.c`,
 * which reads `data[0]` only and has no `enable_chroma` option).  The
 * `enable_chroma` option is vestigial — retained so callers passing it on
 * the CLI / in model JSONs do not see an option-not-recognised error — and
 * warn-on-true here surfaces the no-op behaviour instead of silently
 * producing luma-only output that contradicts the request. See ADR-0597.
 */
static void vif_drop_vestigial_chroma_option(VifStateCuda *s)
{
    if (!s->enable_chroma)
        return;
    vmaf_log(VMAF_LOG_LEVEL_WARNING, "integer_vif (CUDA): enable_chroma=true requested but VIF is "
                                     "luma-only by design (matches CPU integer_vif and upstream "
                                     "Netflix/vmaf); option is a no-op. See ADR-0597.\n");
    s->enable_chroma = false;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    VifStateCuda *s = fex->priv;

    /* Direct `vif_cuda` requests get no fallback (ADR-1324): refuse before
     * touching the CUDA state, so there is nothing to release. */
    const unsigned min_dim = vif_cuda_min_dim();
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vif_cuda requires width >= %u and height >= %u (got %ux%u); the CPU "
                 "extractor `vif` computes smaller frames\n",
                 min_dim, min_dim, w, h);
        return -EINVAL;
    }

    CudaFunctions *cu_f = fex->cu_state->f;
    const int cuda_err = vif_init_cuda_context(fex, s, cu_f);
    if (cuda_err)
        return cuda_err;

    /* The statistic reads its logarithms from the module's table; no frame
     * may be submitted before it holds the CPU's values (ADR-1462). */
    const int table_err = vmaf_cuda_vif_upload_log2_table(fex->cu_state, s->filter1d_module);
    if (table_err)
        return vif_init_unwind(fex, s, table_err);

    /* The names follow the options as the caller set them, before the no-op
     * `enable_chroma` is cleared below: `enable_chroma=true` gives the
     * `_enable_chroma` names, as every extractor names its features from its
     * options first in init() (ADR-1836). A later failure releases the
     * dictionary through vif_init_unwind(). */
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return vif_init_unwind(fex, s, -ENOMEM);

    vif_drop_vestigial_chroma_option(s);
    (void)pix_fmt; /* YUV400P needs no special case — luma-only path handles it. */
    s->n_planes = 1;

    const bool hbd = bpc > 8;

    int tex_alignment;
    const CUresult attr_res = cu_f->cuDeviceGetAttribute(
        &tex_alignment, CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT, fex->cu_state->dev);
    if (attr_res != CUDA_SUCCESS)
        return vif_init_unwind(fex, s, vmaf_cuda_result_to_errno((int)attr_res));

    return vif_setup_buffers(fex, s, w, h, tex_alignment, hbd);
}

static int filter1d_8(VifStateCuda *s, VifBufferCuda *buf, uint8_t *ref_in, uint8_t *dis_in, int w,
                      int h, double vif_enhn_gain_limit, CudaFunctions *cu_f, CUstream stream)
{
    {

        const int size_of_alignment_type = sizeof(uint32_t);
        const int BLOCKX = 128 / size_of_alignment_type;
        const int BLOCKY = 128 / (VMAF_CUDA_CACHE_LINE_SIZE / size_of_alignment_type);
        void *args_vert[] = {
            &*buf, (void *)&ref_in, (void *)&dis_in, &w, &h, (uint16_t *)&vif_filter1d_table};
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

        void *args_hori[] = {&*buf,
                             &w,
                             &h,
                             (uint16_t *)&vif_filter1d_table,
                             &vif_enhn_gain_limit,
                             (void *)&buf->accum};
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
                         (void *)&ref_in,
                         (void *)&dis_in,
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
                         (void *)&ptr};

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
        vif->scale[scale].num = (float)(accum[scale].num_log / 2048.0 + accum[scale].x2 +
                                        (accum[scale].den_non_log -
                                         ((accum[scale].num_non_log) / 16384.0) / (65025.0)));
        vif->scale[scale].den =
            (float)(accum[scale].den_log / 2048.0 - (accum[scale].x + (accum[scale].num_x * 17)) +
                    accum[scale].den_non_log);
    }
}

static int write_scores(write_score_parameters_vif *data)
{
    VmafFeatureCollector *feature_collector = data->feature_collector;
    VifStateCuda *s = data->s;
    unsigned index = data->index;

    VifScore vif;
    vif_reduce_accums((const vif_accums *)data->s->buf.accum_host, &vif);
    VmafVifScoreSet output = {
        .single_precision_ratio = true,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    const unsigned scale_start = s->vif_skip_scale0 ? 1u : 0u;
    for (unsigned scale = 0u; scale < 4u; ++scale) {
        output.scale[(size_t)scale * 2u] = vif.scale[scale].num;
        output.scale[((size_t)scale * 2u) + 1u] = vif.scale[scale].den;
        if (scale >= scale_start) {
            output.score_num += vif.scale[scale].num;
            output.score_den += vif.scale[scale].den;
        }
    }
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "integer_vif_cuda",
                                &output, VMAF_VIF_INTEGER_NAMES, index);
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

/* vif_order_after_scale0 - the private stream waits for the picture stream,
 * whose input buffer scale 0 consumed. */
static int vif_order_after_scale0(VifStateCuda *s, CudaFunctions *cu_f, VmafPicture *ref_pic)
{
    // This event ensures the input buffer is consumed
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->event, vmaf_cuda_picture_get_stream(ref_pic)));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->event, CU_EVENT_WAIT_DEFAULT));
    return 0;
}

/* vif_submit_scales - dispatch the four scales of one plane, halving the
 * size from scale to scale, with the stream hand-off after scale 0.
 *
 * HISS-04: the scale loop of submit_fex_cuda, moved whole.
 */
static int vif_submit_scales(VifStateCuda *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                             VmafPicture *dist_pic, int w, int h)
{
    /* Scale 0 reads the pictures with their own pitches: an imported plane
     * has its producer's, not the texture-aligned pitch init assumed (the
     * import of an arbitrary pitch read the wrong rows, ADR-2023). */
    s->buf.stride = ref_pic->stride[0];
    s->buf.dis_stride = dist_pic->stride[0];
    for (unsigned scale = 0; scale < 4; ++scale) {
        if (scale > 0) {
            w /= 2;
            h /= 2;
        }

        int err = vif_dispatch_filter(s, cu_f, ref_pic, dist_pic, w, h, scale);
        if (!err && scale == 0)
            err = vif_order_after_scale0(s, cu_f, ref_pic);
        if (err)
            return err;
    }
    return 0;
}

/* vif_submit_plane - one plane's four scales and its accumulator readback.
 *
 * HISS-04: the loop body of submit_fex_cuda, moved whole: the same memset,
 * waits, dispatches, event and readback, in the same order.
 *
 * The accumulator reset runs on the picture stream, the stream of the scale 0
 * kernels that add into it (Netflix/vmaf#1305). On the private
 * stream it was ordered against nothing the scale 0 kernels wait for, so
 * under GPU contention it could land after their first atomic adds and erase
 * them. Scales 1 to 3 run on the private stream after an event recorded behind
 * the reset and scale 0, and the readback runs there too.
 */
static int vif_submit_plane(VifStateCuda *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                            VmafPicture *dist_pic, unsigned plane)
{
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->buf.accum_data->data, 0, sizeof(vif_accums) * 4,
                                            vmaf_cuda_picture_get_stream(ref_pic)));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(vmaf_cuda_picture_get_stream(ref_pic),
                                              vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));
    const int err = vif_submit_scales(s, cu_f, ref_pic, dist_pic, (int)ref_pic->w[plane],
                                      (int)dist_pic->h[plane]);
    if (err)
        return err;

    // Queue async download of accumulators
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->buf.accum_host, s->buf.accum_data->data,
                                              sizeof(vif_accums) * 4, s->str));
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->finished, s->str));
    /* Engine-scope fence batching opt-in (T-GPU-OPT-1, ADR-0242). */
    (void)vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    /* n_planes is always 1: VIF is luma-only by design across every backend
     * (matches CPU integer_vif and upstream Netflix/vmaf — see ADR-0597).
     * The loop is retained for shape-parity with the CPU/HIP/SYCL twins. */
    for (unsigned plane = 0; plane < s->n_planes; ++plane) {
        const int err = vif_submit_plane(s, cu_f, ref_pic, dist_pic, plane);
        if (err)
            return err;
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
    return vif_init_unwind(fex, s, 0);
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

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_integer_vif_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_vif_cuda = {.name = "vif_cuda",
                                                  .init = init_fex_cuda,
                                                  .submit = submit_fex_cuda,
                                                  .collect = collect_fex_cuda,
                                                  .flush = flush_fex_cuda,
                                                  .options = options,
                                                  .close = close_fex_cuda,
                                                  .priv_size = sizeof(VifStateCuda),
                                                  .provided_features = provided_features,
                                                  .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
                                                  .context_check = check_context_cuda,
                                                  .context_fallback_name = "vif"};

/* NOLINTEND(modernize-use-nullptr) */
