/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the HIP backend.
 *  Direct port of `libvmaf/src/feature/cuda/integer_psnr_hvs_cuda.c`
 *  (s/cuda/hip/ + HIP API tweaks).
 *
 *  Design mirrors the CUDA twin with the following difference (ADR-1369 port):
 *  - 3 dispatches per frame (Y, Cb, Cr).
 *  - Per-plane single-dispatch design: one HIP block per output 8x8
 *    image block (step=7), 64 threads per block.
 *  - Native samples (uint8_t or uint16_t) are uploaded directly from the
 *    VmafPicture via vmaf_hip_picture_upload(); no host float conversion.
 *    The kernel reads raw integers and skips the float round-trip, saving
 *    4x upload bandwidth at 8 bpc and 2x at 10/12 bpc.
 *  - Combined `psnr_hvs = 0.8*Y + 0.1*(Cb + Cr)` computed on the host
 *    after the per-plane partial-sum readback.
 *  - Rejects YUV400P (no chroma) and bpc > 12 (matches CPU + CUDA).
 *
 *  Without `HAVE_HIPCC` (CPU-only builds, `enable_hip=true` but
 *  `enable_hipcc=false`), `init()` returns -ENOSYS so the feature
 *  engine reports "runtime not ready" rather than crashing.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "integer_psnr_hvs_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

extern const unsigned char psnr_hvs_score_hsaco[];
extern const unsigned int psnr_hvs_score_hsaco_len;
#endif /* HAVE_HIPCC */

#define PSNR_HVS_BLOCK 8
#define PSNR_HVS_STEP 7
#define PSNR_HVS_NUM_PLANES 3
#define PSNR_HVS_BLOCK_DIM 8

typedef struct PsnrHvsStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];
    unsigned bpc;
    int32_t samplemax_sq;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t func_psnr_hvs;

    /* Per-plane ref / dist device buffers (native uint8 or uint16 samples). */
    void *d_ref[PSNR_HVS_NUM_PLANES];
    void *d_dist[PSNR_HVS_NUM_PLANES];
    /* Per-plane block partial-sum device buffers. */
    float *d_partials[PSNR_HVS_NUM_PLANES];

    /* Pinned host staging for partial readback. */
    float *h_partials[PSNR_HVS_NUM_PLANES];
#endif /* HAVE_HIPCC */

    unsigned index;
    VmafDictionary *feature_name_dict;
} PsnrHvsStateHip;

static const VmafOption options[] = {{0}};

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
#endif /* HAVE_HIPCC */

#ifdef HAVE_HIPCC
/* Releases every per-plane device + pinned allocation made by
 * psnr_hvs_alloc_plane_buffers(). Shared verbatim by the init unwind path and
 * close_fex_hip() so both free the same set in the same order (HISS-01). */
static void psnr_hvs_free_plane_buffers(PsnrHvsStateHip *s)
{
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        if (s->d_ref[p]) {
            (void)hipFree(s->d_ref[p]);
            s->d_ref[p] = NULL;
        }
        if (s->d_dist[p]) {
            (void)hipFree(s->d_dist[p]);
            s->d_dist[p] = NULL;
        }
        if (s->d_partials[p]) {
            (void)hipFree(s->d_partials[p]);
            s->d_partials[p] = NULL;
        }
        if (s->h_partials[p]) {
            (void)hipHostFree(s->h_partials[p]);
            s->h_partials[p] = NULL;
        }
    }
}

/* Allocates the per-plane device + pinned staging buffers. On the first
 * failure it returns -ENOMEM and leaves the partially-filled state for the
 * caller's unwind tier to release, exactly as the former goto ladder did. */
static int psnr_hvs_alloc_plane_buffers(PsnrHvsStateHip *s)
{
    const unsigned bpc_bytes = (s->bpc <= 8u ? 1u : 2u);
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        const size_t uint_bytes = (size_t)s->width[p] * s->height[p] * bpc_bytes;
        const size_t partials_bytes = (size_t)s->num_blocks[p] * sizeof(float);

        /* Device buffers hold native samples (1 or 2 bytes each). */
        if (hipMalloc(&s->d_ref[p], uint_bytes) != hipSuccess)
            return -ENOMEM;
        if (hipMalloc(&s->d_dist[p], uint_bytes) != hipSuccess)
            return -ENOMEM;
        if (hipMalloc((void **)&s->d_partials[p], partials_bytes) != hipSuccess)
            return -ENOMEM;

        if (hipHostMalloc((void **)&s->h_partials[p], partials_bytes, hipHostMallocDefault) !=
            hipSuccess)
            return -ENOMEM;
    }
    return 0;
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
    psnr_hvs_free_plane_buffers(s);
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return psnr_hvs_unwind_lc(s, err);
}
#endif /* HAVE_HIPCC */

/* Derives the per-plane dimensions and block counts for `pix_fmt`.
 * Extracted from init_fex_hip() to keep that function inside the HISS-04
 * 60-LOC bound; the arithmetic is copied statement-for-statement. */
static int psnr_hvs_set_plane_geometry(PsnrHvsStateHip *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                       unsigned h)
{
    s->width[0] = w;
    s->height[0] = h;
    switch (pix_fmt) {
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

    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        if (s->width[p] < (unsigned)PSNR_HVS_BLOCK || s->height[p] < (unsigned)PSNR_HVS_BLOCK) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "psnr_hvs_hip: plane %d dims %ux%u smaller than 8x8 block\n", p, s->width[p],
                     s->height[p]);
            return -EINVAL;
        }
        s->num_blocks_x[p] = (s->width[p] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1u;
        s->num_blocks_y[p] = (s->height[p] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1u;
        s->num_blocks[p] = s->num_blocks_x[p] * s->num_blocks_y[p];
    }

    return 0;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    PsnrHvsStateHip *s = fex->priv;

    if (bpc > 12u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_hip: invalid bitdepth (%u); bpc must be <= 12\n",
                 bpc);
        return -EINVAL;
    }
    if (pix_fmt == VMAF_PIX_FMT_YUV400P) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "psnr_hvs_hip: YUV400P unsupported (psnr_hvs needs all 3 planes)\n");
        return -EINVAL;
    }
    if (w < (unsigned)PSNR_HVS_BLOCK || h < (unsigned)PSNR_HVS_BLOCK) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_hip: input %ux%u smaller than 8x8 block\n", w, h);
        return -EINVAL;
    }

    s->bpc = bpc;
    const int32_t samplemax = (int32_t)((1u << bpc) - 1u);
    s->samplemax_sq = samplemax * samplemax;

    int err = psnr_hvs_set_plane_geometry(s, pix_fmt, w, h);
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

    err = psnr_hvs_alloc_plane_buffers(s);
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
/* Launch the psnr_hvs kernel for one plane, zero partials, and start D2H copy. */
static int psnr_hvs_launch_plane(PsnrHvsStateHip *s, int p, int wide, hipStream_t str)
{
    const size_t partials_bytes = (size_t)s->num_blocks[p] * sizeof(float);

    hipError_t rc = hipMemsetAsync(s->d_partials[p], 0, partials_bytes, str);
    if (rc != hipSuccess)
        return psnr_hvs_hip_rc(rc);

    unsigned nbx = s->num_blocks_x[p];
    unsigned nby = s->num_blocks_y[p];
    unsigned width = s->width[p];
    unsigned height = s->height[p];
    int plane_arg = p;
    int wide_arg = wide;

    void *args[] = {
        (void *)&s->d_ref[p], (void *)&s->d_dist[p], (void *)&s->d_partials[p],
        (void *)&width,       (void *)&height,       (void *)&nbx,
        (void *)&nby,         (void *)&plane_arg,    (void *)&wide_arg,
    };
    rc = hipModuleLaunchKernel(s->func_psnr_hvs, nbx, nby, 1, PSNR_HVS_BLOCK_DIM,
                               PSNR_HVS_BLOCK_DIM, 1, 0, str, args, NULL);
    if (rc != hipSuccess)
        return psnr_hvs_hip_rc(rc);

    rc = hipMemcpyAsync(s->h_partials[p], s->d_partials[p], partials_bytes, hipMemcpyDeviceToHost,
                        str);
    if (rc != hipSuccess)
        return psnr_hvs_hip_rc(rc);
    return 0;
}

/* Upload raw native samples and launch the psnr_hvs kernel on each plane.
 * ref_pic / dist_pic are VmafPictures with pageable host data.
 * vmaf_hip_picture_upload() copies and waits before returning (prevents the
 * pageable-upload race documented in picture_hip.h). */
static int launch_psnr_hvs(PsnrHvsStateHip *s, VmafPicture *ref_pic, VmafPicture *dist_pic)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    const unsigned bpc_bytes = (s->bpc <= 8u ? 1u : 2u);
    const int wide = (s->bpc > 8u) ? 1 : 0;

    VmafHipPlaneUpload uploads[2u * PSNR_HVS_NUM_PLANES];
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        const size_t row_bytes = (size_t)s->width[p] * bpc_bytes;
        const size_t i = (size_t)p * 2u;
        uploads[i] = (VmafHipPlaneUpload){.dst = s->d_ref[p],
                                          .dst_pitch = row_bytes,
                                          .pic = ref_pic,
                                          .plane = (unsigned)p,
                                          .row_bytes = row_bytes,
                                          .rows = s->height[p]};
        uploads[i + 1u] = (VmafHipPlaneUpload){.dst = s->d_dist[p],
                                               .dst_pitch = row_bytes,
                                               .pic = dist_pic,
                                               .plane = (unsigned)p,
                                               .row_bytes = row_bytes,
                                               .rows = s->height[p]};
    }
    int err = vmaf_hip_picture_upload(uploads, 2u * PSNR_HVS_NUM_PLANES, s->lc.str);
    if (err != 0)
        return err;

    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        err = psnr_hvs_launch_plane(s, p, wide, str);
        if (err != 0)
            return err;
    }
    return 0;
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
    /* Upload native samples to the device, launch the kernel, and read back
     * partial sums. vmaf_hip_picture_upload() (called inside launch_psnr_hvs)
     * blocks until the copies are done reading the picture, so by the time
     * submit_fex_hip returns the pictures can be recycled safely. */
    int err = launch_psnr_hvs(s, ref_pic, dist_pic);
    if (err != 0)
        return err;

    /* Record the submit event on the kernel stream. vmaf_hip_kernel_submit_post_record
     * records the finished event so collect() can wait for it. */
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipError_t rc = hipEventRecord(submit_ev, str);
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

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    PsnrHvsStateHip *s = fex->priv;

    int wait_err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (wait_err != 0)
        return wait_err;

#ifdef HAVE_HIPCC
    double plane_score[PSNR_HVS_NUM_PLANES];
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        float ret = 0.0f;
        for (unsigned i = 0; i < s->num_blocks[p]; i++)
            ret += s->h_partials[p][i];
        const int pixels = (int)(s->num_blocks[p] * 64u);
        ret /= (float)pixels;
        ret /= (float)s->samplemax_sq;
        plane_score[p] = (double)ret;
    }

    int err = 0;
    static const char *plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                              "psnr_hvs_cr"};
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        const double db = 10.0 * (-1.0 * log10(plane_score[p]));
        err |= vmaf_feature_collector_append(feature_collector, plane_features[p], db, index);
    }
    const double combined = 0.8 * plane_score[0] + 0.1 * (plane_score[1] + plane_score[2]);
    const double db_combined = 10.0 * (-1.0 * log10(combined));
    err |= vmaf_feature_collector_append(feature_collector, "psnr_hvs", db_combined, index);
    return err;
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
    psnr_hvs_free_plane_buffers(s);
    if (s->module != NULL) {
        hipError_t hip_err = hipModuleUnload(s->module);
        if (hip_err != hipSuccess && rc == 0)
            rc = -EIO;
        s->module = NULL;
    }
#endif /* HAVE_HIPCC */

    if (s->feature_name_dict != NULL) {
        int err = vmaf_dictionary_free(&s->feature_name_dict);
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
            .n_dispatches_per_frame = 3,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
