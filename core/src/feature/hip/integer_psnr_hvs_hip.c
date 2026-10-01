/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the HIP backend (port of the ADR-1369
 *  kernel of the SYCL twin to HIP; exact terms per ADR-1397 / ADR-1401).
 *
 *  Per frame, on the extractor's private stream:
 *  - submit() packs the six raw planes (Y, Cb, Cr of both pictures; uint8_t
 *    at 8 bpc, uint16_t above) into extractor-owned pinned buffers and
 *    enqueues their copies to the device, one dispatch of
 *    `psnr_hvs_score.hip` that covers every block of every plane (two
 *    work-items per 8x8 block, step 7), and one copy of the terms back.
 *    The pictures are not read after submit() returns, and submit() does
 *    not wait for the device.
 *  - collect() waits once and emits `psnr_hvs_y/cb/cr` and
 *    `psnr_hvs = 0.8*Y + 0.1*(Cb + Cr)`.
 *  - With `enable_chroma=false`, or for 4:0:0 input, only the luma plane is
 *    staged, dispatched and scored, and `psnr_hvs` is the luma score, as in
 *    the CPU extractor (third_party/xiph/psnr_hvs.c::init).
 *  - Rejects bpc > 12.
 *
 *  ADR-1397: the scores are the CPU extractor's bit for bit. The kernel
 *  stores the 64 masked coefficient errors of every block, computed in the
 *  arithmetic of calc_psnrhvs() (third_party/xiph/psnr_hvs.c), and
 *  psnr_hvs_plane_scores() hands each plane's terms to
 *  vmaf_psnr_hvs_plane_score(), which adds them into one running float in the
 *  CPU's order. The readback is 256 bytes per block (about 65 MB for a
 *  3840x2160 4:2:0 frame); summing per block on the device is cheaper but
 *  rounds differently from the CPU.
 *
 *  Without `HAVE_HIPCC` (CPU-only builds, `enable_hip=true` but
 *  `enable_hipcc=false`), `init()` returns -ENOSYS so the feature
 *  engine reports "runtime not ready" rather than crashing.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/psnr_hvs_score.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "integer_psnr_hvs_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#endif /* HAVE_HIPCC */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define PSNR_HVS_BLOCK 8
#define PSNR_HVS_STEP 7
#define PSNR_HVS_NUM_PLANES PSNR_HVS_HIP_NUM_PLANES

_Static_assert(PSNR_HVS_HIP_TERMS == VMAF_PSNR_HVS_TERMS_PER_BLOCK,
               "the kernel stores what vmaf_psnr_hvs_plane_score() sums per block");

typedef struct PsnrHvsStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];
    /* Offset of each plane's blocks in the one term buffer, in blocks. */
    unsigned first_block[PSNR_HVS_NUM_PLANES];
    unsigned total_blocks;
    size_t row_bytes[PSNR_HVS_NUM_PLANES];
    unsigned bpc;
    /* enable_chroma: when false, only the luma plane is scored. Default true,
     * as in the CPU extractor. */
    bool enable_chroma;
    /* Planes staged, dispatched and scored: 1 when enable_chroma is false or
     * the input is 4:0:0, else 3. */
    unsigned n_planes;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t func_psnr_hvs;

    /* Raw samples of both pictures: device copies and their pinned
     * staging, [plane]. */
    void *d_ref[PSNR_HVS_NUM_PLANES];
    void *d_dist[PSNR_HVS_NUM_PLANES];
    void *h_ref[PSNR_HVS_NUM_PLANES];
    void *h_dist[PSNR_HVS_NUM_PLANES];
    /* PSNR_HVS_HIP_TERMS masked coefficient errors per block, every plane,
     * and their pinned copy. */
    float *d_terms;
    float *h_terms;
#endif /* HAVE_HIPCC */

    unsigned index;
    VmafDictionary *feature_name_dict;
} PsnrHvsStateHip;

/* Planes the twin stages, dispatches and scores: s->n_planes, never more than
 * the three the state's arrays hold. */
static unsigned psnr_hvs_plane_count(const PsnrHvsStateHip *s)
{
    return s->n_planes < (unsigned)PSNR_HVS_NUM_PLANES ? s->n_planes :
                                                         (unsigned)PSNR_HVS_NUM_PLANES;
}

static const VmafOption options[] = {
    {
        .name = "enable_chroma",
        .help = "enable psnr_hvs calculation for chroma channels (Cb and Cr); "
                "when false only the luma plane is scored and psnr_hvs equals "
                "psnr_hvs_y",
        .offset = offsetof(PsnrHvsStateHip, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {0}};

#ifdef HAVE_HIPCC
static int psnr_hvs_hip_rc(hipError_t rc)
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

static int psnr_hvs_hip_module_load(PsnrHvsStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, psnr_hvs_score_hsaco);
    if (rc != hipSuccess)
        return psnr_hvs_hip_rc(rc);

    rc = hipModuleGetFunction(&s->func_psnr_hvs, s->module, "psnr_hvs_hip");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return psnr_hvs_hip_rc(rc);
    }
    return 0;
}

static void psnr_hvs_free_device(void **slot)
{
    if (*slot != NULL) {
        (void)hipFree(*slot);
        *slot = NULL;
    }
}

static void psnr_hvs_free_pinned(void **slot)
{
    if (*slot != NULL) {
        (void)hipHostFree(*slot);
        *slot = NULL;
    }
}

/* Releases every device + pinned allocation made by psnr_hvs_alloc_buffers().
 * Null-guarded, so the init unwind path and close_fex_hip() share it after a
 * partial allocation as well as a full one (HISS-01). */
static void psnr_hvs_free_buffers(PsnrHvsStateHip *s)
{
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        psnr_hvs_free_device(&s->d_ref[p]);
        psnr_hvs_free_device(&s->d_dist[p]);
        psnr_hvs_free_pinned(&s->h_ref[p]);
        psnr_hvs_free_pinned(&s->h_dist[p]);
    }
    psnr_hvs_free_device((void **)&s->d_terms);
    psnr_hvs_free_pinned((void **)&s->h_terms);
}

/* Bytes of the term buffer: PSNR_HVS_HIP_TERMS floats per block of every
 * plane. */
static size_t psnr_hvs_terms_bytes(const PsnrHvsStateHip *s)
{
    return (size_t)s->total_blocks * (size_t)PSNR_HVS_HIP_TERMS * sizeof(float);
}

/* One device and one pinned buffer of `bytes`; -ENOMEM on the first failure,
 * leaving what was allocated for the caller's unwind tier to release. */
static int psnr_hvs_alloc_pair(void **device, void **pinned, size_t bytes)
{
    if (hipMalloc(device, bytes) != hipSuccess) {
        *device = NULL;
        return -ENOMEM;
    }
    if (hipHostMalloc(pinned, bytes, hipHostMallocDefault) != hipSuccess) {
        *pinned = NULL;
        return -ENOMEM;
    }
    return 0;
}

static int psnr_hvs_alloc_buffers(PsnrHvsStateHip *s)
{
    int err = 0;
    for (unsigned p = 0; p < psnr_hvs_plane_count(s) && !err; p++) {
        const size_t bytes = s->row_bytes[p] * s->height[p];
        err = psnr_hvs_alloc_pair(&s->d_ref[p], &s->h_ref[p], bytes);
        if (!err)
            err = psnr_hvs_alloc_pair(&s->d_dist[p], &s->h_dist[p], bytes);
    }
    if (!err) {
        err = psnr_hvs_alloc_pair((void **)&s->d_terms, (void **)&s->h_terms,
                                  psnr_hvs_terms_bytes(s));
    }
    return err;
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init failure unwind — cascading tiers replacing the goto ladder.    */
/* Each tier releases exactly its own acquisition then delegates to    */
/* the next-earlier tier, preserving the fall-through release order.   */
/* ------------------------------------------------------------------ */

static int psnr_hvs_unwind_ctx(PsnrHvsStateHip *s, int err)
{
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int psnr_hvs_unwind_lc(PsnrHvsStateHip *s, int err)
{
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    return psnr_hvs_unwind_ctx(s, err);
}

#ifdef HAVE_HIPCC
static int psnr_hvs_unwind_module(PsnrHvsStateHip *s, int err)
{
    psnr_hvs_free_buffers(s);
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return psnr_hvs_unwind_lc(s, err);
}
#endif /* HAVE_HIPCC */

/* Per-plane dimensions for `pix_fmt`, by picture.c's ceil rule, and the number
 * of planes to score. */
static int psnr_hvs_set_plane_dims(PsnrHvsStateHip *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                   unsigned h)
{
    s->width[0] = w;
    s->height[0] = h;
    /* 4:0:0 has no chroma planes: luma only whatever enable_chroma says, as in
     * the CPU extractor (third_party/xiph/psnr_hvs.c::init). */
    s->n_planes =
        (s->enable_chroma && pix_fmt != VMAF_PIX_FMT_YUV400P) ? (unsigned)PSNR_HVS_NUM_PLANES : 1u;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_YUV400P:
        s->width[1] = s->width[2] = 0u;
        s->height[1] = s->height[2] = 0u;
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
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_hip: unsupported pix_fmt\n");
        return -EINVAL;
    }
    return 0;
}

/* Block grid of every active plane and the planes' offsets in the one term
 * buffer the single dispatch writes. */
static int psnr_hvs_set_plane_geometry(PsnrHvsStateHip *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                       unsigned h)
{
    const int err = psnr_hvs_set_plane_dims(s, pix_fmt, w, h);
    if (err != 0)
        return err;

    const size_t bytes_per_sample = (s->bpc > 8u) ? 2u : 1u;
    s->total_blocks = 0u;
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        if (s->width[p] < (unsigned)PSNR_HVS_BLOCK || s->height[p] < (unsigned)PSNR_HVS_BLOCK) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "psnr_hvs_hip: plane %u dims %ux%u smaller than 8x8 block\n", p, s->width[p],
                     s->height[p]);
            return -EINVAL;
        }
        s->num_blocks_x[p] = (s->width[p] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1u;
        s->num_blocks_y[p] = (s->height[p] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1u;
        s->num_blocks[p] = s->num_blocks_x[p] * s->num_blocks_y[p];
        s->first_block[p] = s->total_blocks;
        s->total_blocks += s->num_blocks[p];
        s->row_bytes[p] = (size_t)s->width[p] * bytes_per_sample;
    }
    return 0;
}

static int psnr_hvs_validate_input(unsigned bpc, unsigned w, unsigned h)
{
    if (bpc > 12u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_hip: invalid bitdepth (%u); bpc must be <= 12\n",
                 bpc);
        return -EINVAL;
    }
    if (w < (unsigned)PSNR_HVS_BLOCK || h < (unsigned)PSNR_HVS_BLOCK) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_hip: input %ux%u smaller than 8x8 block\n", w, h);
        return -EINVAL;
    }
    return 0;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    PsnrHvsStateHip *s = fex->priv;

    int err = psnr_hvs_validate_input(bpc, w, h);
    if (err != 0)
        return err;

    s->bpc = bpc;

    err = psnr_hvs_set_plane_geometry(s, pix_fmt, w, h);
    if (err != 0)
        return err;

    err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        return psnr_hvs_unwind_ctx(s, err);

#ifdef HAVE_HIPCC
    err = psnr_hvs_hip_module_load(s);
    if (err != 0)
        return psnr_hvs_unwind_lc(s, err);

    err = psnr_hvs_alloc_buffers(s);
    if (err != 0)
        return psnr_hvs_unwind_module(s, err);
#endif /* HAVE_HIPCC */

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
#ifdef HAVE_HIPCC
        return psnr_hvs_unwind_module(s, -ENOMEM);
#else
        return psnr_hvs_unwind_lc(s, -ENOMEM);
#endif
    }
    return 0;
}

#ifdef HAVE_HIPCC
static int psnr_hvs_picture_matches(const PsnrHvsStateHip *s, const VmafPicture *pic)
{
    if (pic == NULL || pic->bpc != s->bpc)
        return 0;
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        if (pic->data[p] == NULL || pic->w[p] != s->width[p] || pic->h[p] != s->height[p])
            return 0;
    }
    return 1;
}

/* Pack one plane's rows into pinned staging. The picture goes back to the
 * caller when submit() returns, so the device copy must not read it
 * (core/src/feature/hip/AGENTS.md, "Picture uploads"); the staging buffer is
 * the extractor's until the next submit(), which follows collect(). */
static void psnr_hvs_stage_plane(const PsnrHvsStateHip *s, const VmafPicture *pic, unsigned p,
                                 void *dst)
{
    const uint8_t *src = (const uint8_t *)pic->data[p];
    uint8_t *out = (uint8_t *)dst;
    const size_t row_bytes = s->row_bytes[p];
    const size_t stride = (size_t)pic->stride[p];
    if (stride == row_bytes) {
        memcpy(out, src, row_bytes * s->height[p]);
        return;
    }
    for (unsigned row = 0; row < s->height[p]; row++)
        memcpy(out + (size_t)row * row_bytes, src + (size_t)row * stride, row_bytes);
}

static int psnr_hvs_enqueue_uploads(const PsnrHvsStateHip *s, hipStream_t str)
{
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        const size_t bytes = s->row_bytes[p] * s->height[p];
        hipError_t rc = hipMemcpyAsync(s->d_ref[p], s->h_ref[p], bytes, hipMemcpyHostToDevice, str);
        if (rc == hipSuccess)
            rc = hipMemcpyAsync(s->d_dist[p], s->h_dist[p], bytes, hipMemcpyHostToDevice, str);
        if (rc != hipSuccess)
            return psnr_hvs_hip_rc(rc);
    }
    return 0;
}

/* The frame's device work: raw planes up, one dispatch over every block of
 * every plane, the terms back. */
static int psnr_hvs_enqueue_frame(const PsnrHvsStateHip *s)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    int err = psnr_hvs_enqueue_uploads(s, str);
    if (err != 0)
        return err;

    struct PsnrHvsHipKernelArgs args;
    (void)memset(&args, 0, sizeof(args));
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        args.plane[p].ref = s->d_ref[p];
        args.plane[p].dist = s->d_dist[p];
        args.plane[p].width = s->width[p];
        args.plane[p].blocks_x = s->num_blocks_x[p];
        args.plane[p].first_block = s->first_block[p];
    }
    args.terms = s->d_terms;
    args.n_planes = psnr_hvs_plane_count(s);
    args.total_blocks = s->total_blocks;
    args.wide = (s->bpc > 8u) ? 1u : 0u;
    void *params[] = {&args};
    const size_t items = 2u * (size_t)s->total_blocks;
    const unsigned groups = (unsigned)((items + PSNR_HVS_HIP_WG - 1u) / PSNR_HVS_HIP_WG);
    hipError_t rc = hipModuleLaunchKernel(s->func_psnr_hvs, groups, 1, 1, PSNR_HVS_HIP_WG, 1, 1, 0,
                                          str, params, NULL);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->h_terms, s->d_terms, psnr_hvs_terms_bytes(s), hipMemcpyDeviceToHost,
                            str);
    }
    return psnr_hvs_hip_rc(rc);
}
#endif /* HAVE_HIPCC */

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    PsnrHvsStateHip *s = fex->priv;
    s->index = index;

#ifdef HAVE_HIPCC
    if (!psnr_hvs_picture_matches(s, ref_pic) || !psnr_hvs_picture_matches(s, dist_pic))
        return -EINVAL;
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        psnr_hvs_stage_plane(s, ref_pic, p, s->h_ref[p]);
        psnr_hvs_stage_plane(s, dist_pic, p, s->h_dist[p]);
    }
    const int err = psnr_hvs_enqueue_frame(s);
    if (err != 0)
        return err;

    /* Record the submit event on the kernel stream (single-stream HIP
     * posture, as float_psnr_hip). vmaf_hip_kernel_submit_post_record
     * records the finished event so collect() can wait for it. */
    const hipError_t rc =
        hipEventRecord(vmaf_hip_event_of(s->lc.submit), vmaf_hip_stream_of(s->lc.str));
    if (rc != hipSuccess)
        return psnr_hvs_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
#else
    (void)ref_pic;
    (void)dist_pic;
    /* Scaffold posture (enable_hipcc=false): report not-implemented, which is
     * the contract `meson_options.txt` documents and every HIP parity test
     * skips on.
     *
     * This used to call `vmaf_hip_kernel_submit_pre_launch(&s->lc, s->ctx,
     * NULL, ...)` first and return its result on error. That call passes
     * `rb == NULL`, which the helper rejects outright, so it ALWAYS returned
     * -EINVAL and the `-ENOSYS` below was unreachable. The extractor therefore
     * failed instead of skipping on every default-configured HIP build, and
     * `test_hip_psnr_hvs_parity` / `..._large` failed with it. The call did
     * nothing else: the NULL check is the helper's first statement, ahead of
     * any work. */
    (void)s;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* Each plane's score from its terms, added in the CPU's order (ADR-1397). */
static void psnr_hvs_plane_scores(const PsnrHvsStateHip *s, double plane_score[])
{
    // SAFETY: s->h_terms holds PSNR_HVS_HIP_TERMS floats for each of the
    // s->total_blocks blocks, and s->first_block[p] + s->num_blocks[p] <=
    // s->total_blocks holds by construction (psnr_hvs_set_plane_geometry).
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        const float *plane_terms =
            s->h_terms + ((size_t)s->first_block[p] * (size_t)PSNR_HVS_HIP_TERMS);
        plane_score[p] = vmaf_psnr_hvs_plane_score(plane_terms, s->num_blocks[p], s->bpc);
    }
}

/* The CPU extract() expressions: dB per plane, then the weighted score. */
static int psnr_hvs_append_scores(VmafFeatureCollector *feature_collector, const PsnrHvsStateHip *s,
                                  const double plane_score[], unsigned index)
{
    static const char *plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                              "psnr_hvs_cr"};
    int err = 0;
    for (unsigned p = 0; p < psnr_hvs_plane_count(s); p++) {
        err |= vmaf_feature_collector_append(feature_collector, plane_features[p],
                                             vmaf_psnr_hvs_score_db(plane_score[p]), index);
    }
    const double combined = vmaf_psnr_hvs_combined_score(plane_score, psnr_hvs_plane_count(s));
    err |= vmaf_feature_collector_append(feature_collector, "psnr_hvs",
                                         vmaf_psnr_hvs_score_db(combined), index);
    return err;
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    PsnrHvsStateHip *s = fex->priv;

    const int wait_err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (wait_err != 0)
        return wait_err;

#ifdef HAVE_HIPCC
    double plane_score[PSNR_HVS_NUM_PLANES] = {0.0, 0.0, 0.0};
    psnr_hvs_plane_scores(s, plane_score);
    return psnr_hvs_append_scores(feature_collector, s, plane_score, index);
#else
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    PsnrHvsStateHip *s = fex->priv;
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);

#ifdef HAVE_HIPCC
    psnr_hvs_free_buffers(s);
    if (s->module != NULL) {
        const hipError_t hip_err = hipModuleUnload(s->module);
        if (hip_err != hipSuccess && rc == 0)
            rc = -EIO;
        s->module = NULL;
    }
#endif /* HAVE_HIPCC */

    if (s->feature_name_dict != NULL) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    if (s->ctx != NULL) {
        vmaf_hip_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static const char *provided_features[] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr", "psnr_hvs",
                                          NULL};

/* Load-bearing: registered in feature_extractor.c's feature_extractor_list[].
 * Making this static would unlink the extractor from the registry. Same
 * pattern as every other HIP consumer (see integer_psnr_hip.c). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_psnr_hvs_hip = {
    .name = "psnr_hvs_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(PsnrHvsStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
