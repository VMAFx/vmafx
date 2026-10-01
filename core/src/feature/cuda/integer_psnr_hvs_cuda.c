/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the CUDA backend
 *  (T7-23 / ADR-0188 / ADR-0191 / ADR-1369 / ADR-1397).
 *
 *  CUDA port of ADR-1369: the kernel reads the raw 8- to 12-bit samples of
 *  the device pictures with their pitch (no host copy, host conversion or
 *  private upload), two threads per 8x8 block exchange their statistics with
 *  a warp shuffle, the integer DCT runs in shared memory, and one launch
 *  covers every active plane. 4:0:0 input is luma only, as in the CPU
 *  extractor.
 *
 *  ADR-1397: the scores are the CPU extractor's bit for bit. The kernel
 *  stores the 64 masked coefficient errors of every block, computed in the
 *  arithmetic of calc_psnrhvs() (third_party/xiph/psnr_hvs.c), and
 *  reduce_hvs_planes() hands each plane's terms to
 *  vmaf_psnr_hvs_plane_score(), which adds them into one running float in the
 *  CPU's order. The readback is 256 bytes per block (about 65 MB for a
 *  3840x2160 4:2:0 frame); summing per block on the device is cheaper but
 *  rounds differently from the CPU.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "cuda/integer_psnr_hvs_cuda.h"
#include "cuda/kernel_template.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/psnr_hvs_score.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

_Static_assert(PSNR_HVS_TERMS == VMAF_PSNR_HVS_TERMS_PER_BLOCK,
               "the kernel stores what vmaf_psnr_hvs_plane_score() sums per block");

typedef struct PsnrHvsScratchLayout {
    size_t raw_terms_offset;
    size_t block_masks_offset;
    size_t block_counts_offset;
    size_t chunk_totals_offset;
    size_t chunk_offsets_offset;
    size_t header_offset;
    size_t total_bytes;
    unsigned num_chunks;
} PsnrHvsScratchLayout;

typedef struct PsnrHvsStateCuda {
    VmafCudaKernelLifecycle lc;
    VmafCudaKernelReadback rb;
    CUfunction func_psnr_hvs;
    CUfunction func_scan_reduce;
    CUfunction func_scan_prefix;
    CUfunction func_compact;
    CUmodule module;

    PsnrHvsScratchLayout layout;
    VmafCudaBuffer *scratch;
    PsnrHvsHeader *host_header;

    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];
    unsigned first_block[PSNR_HVS_NUM_PLANES];
    unsigned total_blocks;

    unsigned bpc;

    bool enable_chroma;
    unsigned n_planes;

    unsigned index;
    VmafDictionary *feature_name_dict;
} PsnrHvsStateCuda;

static const VmafOption options[] = {
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels",
        .offset = offsetof(PsnrHvsStateCuda, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {0},
};

static int validate_hvs_input(unsigned bpc, unsigned w, unsigned h)
{
    if (bpc > 12u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_cuda: invalid bitdepth (%u); bpc must be <= 12\n",
                 bpc);
        return -EINVAL;
    }
    if (w < (unsigned)PSNR_HVS_BLOCK || h < (unsigned)PSNR_HVS_BLOCK) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_cuda: input %ux%u smaller than 8x8 block\n", w, h);
        return -EINVAL;
    }
    return 0;
}

static int configure_hvs_geometry(PsnrHvsStateCuda *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                  unsigned h)
{
    s->width[0] = w;
    s->height[0] = h;
    /* 4:0:0 has no chroma planes: luma only whatever enable_chroma says, as in
     * the CPU extractor (third_party/xiph/psnr_hvs.c::init). */
    s->n_planes =
        (s->enable_chroma && pix_fmt != VMAF_PIX_FMT_YUV400P) ? (unsigned)PSNR_HVS_NUM_PLANES : 1U;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_YUV400P:
        s->width[1] = s->width[2] = 0U;
        s->height[1] = s->height[2] = 0U;
        break;
    case VMAF_PIX_FMT_YUV420P:
        s->width[1] = s->width[2] = (w + 1u) >> 1;
        s->height[1] = s->height[2] = (h + 1u) >> 1;
        break;
    case VMAF_PIX_FMT_YUV422P:
        s->width[1] = s->width[2] = (w + 1u) >> 1;
        s->height[1] = s->height[2] = h;
        break;
    case VMAF_PIX_FMT_YUV444P:
        s->width[1] = s->width[2] = w;
        s->height[1] = s->height[2] = h;
        break;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_cuda: unsupported pix_fmt\n");
        return -EINVAL;
    }
    return 0;
}

static int configure_hvs_blocks(PsnrHvsStateCuda *s)
{
    s->total_blocks = 0U;
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        if (s->width[plane] < (unsigned)PSNR_HVS_BLOCK ||
            s->height[plane] < (unsigned)PSNR_HVS_BLOCK) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "psnr_hvs_cuda: plane %u dims %ux%u smaller than 8x8 block\n", plane,
                     s->width[plane], s->height[plane]);
            return -EINVAL;
        }
        s->num_blocks_x[plane] = (s->width[plane] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1;
        s->num_blocks_y[plane] = (s->height[plane] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1;
        s->num_blocks[plane] = s->num_blocks_x[plane] * s->num_blocks_y[plane];
        s->first_block[plane] = s->total_blocks;
        s->total_blocks += s->num_blocks[plane];
    }
    return 0;
}

static inline size_t hvs_align256(size_t sz)
{
    return (sz + 255u) & ~((size_t)255u);
}

static PsnrHvsScratchLayout hvs_compute_scratch_layout(unsigned total_blocks)
{
    PsnrHvsScratchLayout l;
    memset(&l, 0, sizeof(l));
    l.num_chunks = (total_blocks + 255u) / 256u;

    size_t off = 0;
    l.raw_terms_offset = off;
    off += hvs_align256((size_t)total_blocks * (size_t)PSNR_HVS_TERMS * sizeof(float));

    l.block_masks_offset = off;
    off += hvs_align256((size_t)total_blocks * sizeof(uint64_t));

    l.block_counts_offset = off;
    off += hvs_align256((size_t)total_blocks * sizeof(uint32_t));

    l.chunk_totals_offset = off;
    off += hvs_align256((size_t)l.num_chunks * sizeof(uint32_t));

    l.chunk_offsets_offset = off;
    off += hvs_align256((size_t)l.num_chunks * sizeof(uint32_t));

    l.header_offset = off;
    off += hvs_align256(sizeof(PsnrHvsHeader));

    l.total_bytes = off;
    return l;
}

/* Bytes of the term buffer: PSNR_HVS_TERMS floats per block of every plane. */
static size_t hvs_terms_bytes(const PsnrHvsStateCuda *s)
{
    return (size_t)s->total_blocks * (size_t)PSNR_HVS_TERMS * sizeof(float);
}

static int psnr_hvs_init_unwind(VmafFeatureExtractor *fex, PsnrHvsStateCuda *s, int cause)
{
    int rc = cause;
    const int phase_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (phase_rc && !rc)
        rc = phase_rc;

    const int rb_rc = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (rb_rc && !rc)
        rc = rb_rc;

    const int sc_rc = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->scratch);
    if (sc_rc && !rc)
        rc = sc_rc;

    const int hh_rc = vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->host_header);
    if (hh_rc && !rc)
        rc = hh_rc;

    const int dict_rc = vmaf_dictionary_free(&s->feature_name_dict);
    if (dict_rc && !rc)
        rc = dict_rc;

    const int mod_rc = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (mod_rc && !rc)
        rc = mod_rc;

    return rc;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    PsnrHvsStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    int err = validate_hvs_input(bpc, w, h);
    if (err)
        return err;

    s->bpc = bpc;

    err = configure_hvs_geometry(s, pix_fmt, w, h);
    if (err)
        return err;
    err = configure_hvs_blocks(s);
    if (err)
        return err;

    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return psnr_hvs_init_unwind(fex, s, err);

    int ctx_pushed = 0;
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, psnr_hvs_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_psnr_hvs, s->module, "psnr_hvs"), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_scan_reduce, s->module, "hvs_scan_reduce"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_scan_prefix, s->module, "hvs_scan_prefix"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_compact, s->module, "hvs_compact"), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);

    err = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, hvs_terms_bytes(s));
    if (err)
        return psnr_hvs_init_unwind(fex, s, err);

    s->layout = hvs_compute_scratch_layout(s->total_blocks);
    err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->scratch, s->layout.total_bytes);
    if (err)
        return psnr_hvs_init_unwind(fex, s, err);

    err =
        vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->host_header, sizeof(PsnrHvsHeader));
    if (err)
        return psnr_hvs_init_unwind(fex, s, err);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return psnr_hvs_init_unwind(fex, s, -ENOMEM);

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return psnr_hvs_init_unwind(fex, s, _cuda_err);
}

static void fill_kernel_args(const PsnrHvsStateCuda *s, const VmafPicture *ref_pic,
                             const VmafPicture *dist_pic, PsnrHvsKernelArgs *args, float *raw_terms,
                             uint64_t *block_masks, uint32_t *block_counts)
{
    memset(args, 0, sizeof(*args));
    for (unsigned p = 0; p < s->n_planes; p++) {
        args->plane[p].ref = ref_pic->data[p];
        args->plane[p].dist = dist_pic->data[p];
        args->plane[p].ref_stride = (size_t)ref_pic->stride[p];
        args->plane[p].dist_stride = (size_t)dist_pic->stride[p];
        args->plane[p].width = s->width[p];
        args->plane[p].blocks_x = s->num_blocks_x[p];
        args->plane[p].first_block = s->first_block[p];
    }
    args->terms = raw_terms;
    args->block_masks = block_masks;
    args->block_counts = block_counts;
    args->n_planes = s->n_planes;
    args->total_blocks = s->total_blocks;
    args->wide = (s->bpc > 8u) ? 1 : 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    PsnrHvsStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    s->index = index;

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CUevent dist_ready = vmaf_cuda_picture_get_ready_event(dist_pic);

    char *base = (char *)s->scratch->data;
    float *raw_terms = (float *)(base + s->layout.raw_terms_offset);
    uint64_t *block_masks = (uint64_t *)(base + s->layout.block_masks_offset);
    uint32_t *block_counts = (uint32_t *)(base + s->layout.block_counts_offset);
    uint32_t *chunk_totals = (uint32_t *)(base + s->layout.chunk_totals_offset);
    uint32_t *chunk_offsets = (uint32_t *)(base + s->layout.chunk_offsets_offset);
    PsnrHvsHeader *d_header = (PsnrHvsHeader *)(base + s->layout.header_offset);
    // NOLINTNEXTLINE(performance-no-int-to-ptr): Driver API device address the kernel dereferences (ADR-0747)
    float *packed_terms = (float *)s->rb.device->data;

    PsnrHvsKernelArgs args;
    fill_kernel_args(s, ref_pic, dist_pic, &args, raw_terms, block_masks, block_counts);

    int err =
        vmaf_cuda_kernel_submit_pre_launch(&s->lc, fex->cu_state, &s->rb, pic_stream, dist_ready);
    if (err)
        return err;

    const size_t items = 2U * (size_t)s->total_blocks;
    const unsigned grid_x = (unsigned)((items + (size_t)PSNR_HVS_WG - 1U) / (size_t)PSNR_HVS_WG);
    void *hvs_p[] = {&args};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_psnr_hvs, grid_x, 1u, 1u, PSNR_HVS_WG, 1u, 1u, 0,
                                           pic_stream, hvs_p, NULL));

    void *red_p[] = {&block_counts, &chunk_totals, &s->total_blocks};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_scan_reduce, s->layout.num_chunks, 1u, 1u, 256u,
                                           1u, 1u, 0, pic_stream, red_p, NULL));

    void *pre_p[] = {&chunk_totals, &chunk_offsets, &d_header, &s->layout.num_chunks};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_scan_prefix, 1u, 1u, 1u, 512u, 1u, 1u, 0,
                                           pic_stream, pre_p, NULL));

    void *cmp_p[] = {&args,          &raw_terms,    &block_masks, &block_counts,
                     &chunk_offsets, &packed_terms, &d_header};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_compact, s->layout.num_chunks, 1u, 1u, 256u, 1u,
                                           1u, 0, pic_stream, cmp_p, NULL));

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->host_header, (CUdeviceptr)d_header,
                                              sizeof(PsnrHvsHeader), s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

/* Each plane's score from its terms, added in the CPU's order (ADR-1397). */
static void reduce_hvs_planes(const PsnrHvsStateCuda *s, double scores[PSNR_HVS_NUM_PLANES])
{
    if (!s->host_header) {
        const float *terms = (const float *)s->rb.host_pinned;
        for (unsigned p = 0; p < s->n_planes; p++) {
            const float *plane_terms = terms + (size_t)s->first_block[p] * (size_t)PSNR_HVS_TERMS;
            scores[p] = vmaf_psnr_hvs_plane_score(plane_terms, s->num_blocks[p], s->bpc);
        }
        return;
    }
    const float *compact_terms = (const float *)s->rb.host_pinned;
    for (unsigned p = 0; p < s->n_planes; p++) {
        const uint32_t start = s->host_header->plane_offsets[p];
        const uint32_t end = (p + 1u < s->n_planes) ? s->host_header->plane_offsets[p + 1u] :
                                                      s->host_header->total_terms;
        const size_t n_compact = (size_t)(end - start);
        scores[p] = vmaf_psnr_hvs_plane_score_compacted(compact_terms + start, n_compact,
                                                        s->num_blocks[p], s->bpc);
    }
}

static int append_hvs_scores(VmafFeatureCollector *collector, const PsnrHvsStateCuda *s,
                             const double scores[PSNR_HVS_NUM_PLANES], unsigned index)
{
    static const char *const plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                                    "psnr_hvs_cr"};
    int err = 0;
    for (unsigned p = 0; p < s->n_planes; p++) {
        err |= vmaf_feature_collector_append(collector, plane_features[p],
                                             vmaf_psnr_hvs_score_db(scores[p]), index);
    }
    const double combined = vmaf_psnr_hvs_combined_score(scores, s->n_planes);
    err |= vmaf_feature_collector_append(collector, "psnr_hvs", vmaf_psnr_hvs_score_db(combined),
                                         index);
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    PsnrHvsStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;

    const uint32_t total_terms = s->host_header->total_terms;
    if (total_terms > 0u) {
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                            (size_t)total_terms * sizeof(float), s->lc.str));
        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->lc.str));
    }

    double plane_scores[PSNR_HVS_NUM_PLANES] = {0.0, 0.0, 0.0};
    reduce_hvs_planes(s, plane_scores);
    return append_hvs_scores(feature_collector, s, plane_scores, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    PsnrHvsStateCuda *s = fex->priv;
    return psnr_hvs_init_unwind(fex, s, 0);
}

static const char *provided_features[] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr", "psnr_hvs",
                                          NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_psnr_hvs_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_psnr_hvs_cuda = {
    .name = "psnr_hvs_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(PsnrHvsStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
