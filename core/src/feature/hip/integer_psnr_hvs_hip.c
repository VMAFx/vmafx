/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  psnr_hvs feature extractor on the HIP backend.
 *  Direct port of `libvmaf/src/feature/cuda/integer_psnr_hvs_cuda.c`
 *  (s/cuda/hip/ + HIP API tweaks).
 *
 *  Design mirrors the CUDA twin exactly:
 *  - 3 dispatches per frame (Y, Cb, Cr).
 *  - Per-plane single-dispatch design: one HIP block per output 8x8
 *    image block (step=7), 64 threads per block.
 *  - Host-side uint-to-float normalisation into tightly-pitched device
 *    float buffers (same picture_copy semantics as the CUDA twin).
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
#include "integer_psnr_hvs_hip.h"

#ifdef HAVE_HIPCC
#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

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

    /* Per-plane ref / dist float device buffers (normalised). */
    float *d_ref[PSNR_HVS_NUM_PLANES];
    float *d_dist[PSNR_HVS_NUM_PLANES];
    /* Per-plane block partial-sum device buffers. */
    float *d_partials[PSNR_HVS_NUM_PLANES];

    /* Pinned host staging for float planes. */
    float *h_ref[PSNR_HVS_NUM_PLANES];
    float *h_dist[PSNR_HVS_NUM_PLANES];
    /* Pinned host staging for partial readback. */
    float *h_partials[PSNR_HVS_NUM_PLANES];

    /* Persistent pinned uint8/uint16 staging for device-to-host readback
     * of pic planes (mirrors T-GPU-OPT-3 from the CUDA twin).
     * Sized at init() time to width x height x bpc_bytes per plane. */
    void *h_uint_ref[PSNR_HVS_NUM_PLANES];
    void *h_uint_dist[PSNR_HVS_NUM_PLANES];
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

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        goto fail_after_ctx;

#ifdef HAVE_HIPCC
    err = psnr_hvs_hip_module_load(s);
    if (err != 0)
        goto fail_after_lc;

    const unsigned bpc_bytes = (s->bpc <= 8u ? 1u : 2u);
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        const size_t plane_bytes = (size_t)s->width[p] * s->height[p] * sizeof(float);
        const size_t partials_bytes = (size_t)s->num_blocks[p] * sizeof(float);
        const size_t uint_bytes = (size_t)s->width[p] * s->height[p] * bpc_bytes;

        hipError_t rc = hipMalloc((void **)&s->d_ref[p], plane_bytes);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
        rc = hipMalloc((void **)&s->d_dist[p], plane_bytes);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
        rc = hipMalloc((void **)&s->d_partials[p], partials_bytes);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }

        rc = hipHostMalloc((void **)&s->h_ref[p], plane_bytes, hipHostMallocDefault);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
        rc = hipHostMalloc((void **)&s->h_dist[p], plane_bytes, hipHostMallocDefault);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
        rc = hipHostMalloc((void **)&s->h_partials[p], partials_bytes, hipHostMallocDefault);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
        rc = hipHostMalloc(&s->h_uint_ref[p], uint_bytes, hipHostMallocDefault);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
        rc = hipHostMalloc(&s->h_uint_dist[p], uint_bytes, hipHostMallocDefault);
        if (rc != hipSuccess) {
            err = -ENOMEM;
            goto fail_after_module;
        }
    }
#endif /* HAVE_HIPCC */

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
#ifdef HAVE_HIPCC
        goto fail_after_module;
#else
        goto fail_after_lc;
#endif
    }
    return 0;

#ifdef HAVE_HIPCC
fail_after_module:
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
        if (s->h_ref[p]) {
            (void)hipHostFree(s->h_ref[p]);
            s->h_ref[p] = NULL;
        }
        if (s->h_dist[p]) {
            (void)hipHostFree(s->h_dist[p]);
            s->h_dist[p] = NULL;
        }
        if (s->h_partials[p]) {
            (void)hipHostFree(s->h_partials[p]);
            s->h_partials[p] = NULL;
        }
        if (s->h_uint_ref[p]) {
            (void)hipHostFree(s->h_uint_ref[p]);
            s->h_uint_ref[p] = NULL;
        }
        if (s->h_uint_dist[p]) {
            (void)hipHostFree(s->h_uint_dist[p]);
            s->h_uint_dist[p] = NULL;
        }
    }
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
#endif /* HAVE_HIPCC */
fail_after_lc:
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
fail_after_ctx:
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

#ifdef HAVE_HIPCC
/* Normalise a uint8/uint16 plane from the VmafPicture (already D2H on the
 * CPU side here — we do a simple 2D memcpy from pic->data bypassing the
 * CUDA D2H trick, since HIP picture-stream integration is post-scaffold).
 * The arithmetic mirrors picture_copy.c exactly so scores match the
 * CUDA twin. */
static void upload_plane(PsnrHvsStateHip *s, const VmafPicture *pic, int plane)
{
    const unsigned bpc_bytes = (s->bpc <= 8u ? 1u : 2u);
    const unsigned W = s->width[plane];
    const unsigned H = s->height[plane];

    if (s->bpc <= 8u) {
        const uint8_t *src = (const uint8_t *)pic->data[plane];
        const ptrdiff_t stride = pic->stride[plane];
        uint8_t *dst_ref = (uint8_t *)s->h_uint_ref[plane];
        uint8_t *dst_dist = (uint8_t *)s->h_uint_dist[plane];
        /* Only one of ref/dist is passed; caller calls twice. */
        (void)dst_dist;
        /* This helper is split: call with ref_pic and dist_pic separately. */
        for (unsigned y = 0; y < H; y++) {
            for (unsigned x = 0; x < W; x++) {
                s->h_ref[plane][y * W + x] = (float)src[y * stride + x];
            }
        }
    } else {
        const float scaler = (s->bpc == 10) ? 4.0f : (s->bpc == 12) ? 16.0f : 1.0f;
        const uint16_t *src = (const uint16_t *)pic->data[plane];
        const ptrdiff_t stride_u16 = (ptrdiff_t)pic->stride[plane] / (ptrdiff_t)sizeof(uint16_t);
        for (unsigned y = 0; y < H; y++) {
            for (unsigned x = 0; x < W; x++) {
                s->h_ref[plane][y * W + x] =
                    (float)src[(ptrdiff_t)y * stride_u16 + (ptrdiff_t)x] / scaler;
            }
        }
    }
    (void)bpc_bytes;
}

static void upload_plane_dist(PsnrHvsStateHip *s, const VmafPicture *pic, int plane)
{
    const unsigned W = s->width[plane];
    const unsigned H = s->height[plane];

    if (s->bpc <= 8u) {
        const uint8_t *src = (const uint8_t *)pic->data[plane];
        const ptrdiff_t stride = pic->stride[plane];
        for (unsigned y = 0; y < H; y++) {
            for (unsigned x = 0; x < W; x++) {
                s->h_dist[plane][y * W + x] = (float)src[y * stride + x];
            }
        }
    } else {
        const float scaler = (s->bpc == 10) ? 4.0f : (s->bpc == 12) ? 16.0f : 1.0f;
        const uint16_t *src = (const uint16_t *)pic->data[plane];
        const ptrdiff_t stride_u16 = (ptrdiff_t)pic->stride[plane] / (ptrdiff_t)sizeof(uint16_t);
        for (unsigned y = 0; y < H; y++) {
            for (unsigned x = 0; x < W; x++) {
                s->h_dist[plane][y * W + x] =
                    (float)src[(ptrdiff_t)y * stride_u16 + (ptrdiff_t)x] / scaler;
            }
        }
    }
}

static int launch_psnr_hvs(PsnrHvsStateHip *s)
{
    hipStream_t str = (hipStream_t)s->lc.str;

    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        const size_t plane_bytes = (size_t)s->width[p] * s->height[p] * sizeof(float);
        const size_t partials_bytes = (size_t)s->num_blocks[p] * sizeof(float);

        /* H2D: float ref/dist planes. */
        hipError_t rc =
            hipMemcpyAsync(s->d_ref[p], s->h_ref[p], plane_bytes, hipMemcpyHostToDevice, str);
        if (rc != hipSuccess)
            return psnr_hvs_hip_rc(rc);
        rc = hipMemcpyAsync(s->d_dist[p], s->h_dist[p], plane_bytes, hipMemcpyHostToDevice, str);
        if (rc != hipSuccess)
            return psnr_hvs_hip_rc(rc);

        /* Zero the partial-sum buffer. */
        rc = hipMemsetAsync(s->d_partials[p], 0, partials_bytes, str);
        if (rc != hipSuccess)
            return psnr_hvs_hip_rc(rc);

        /* Launch one block per output 8x8 image block. */
        unsigned nbx = s->num_blocks_x[p];
        unsigned nby = s->num_blocks_y[p];
        unsigned width = s->width[p];
        unsigned height = s->height[p];
        int plane_arg = p;
        int bpc_arg = (int)s->bpc;

        /* hipModuleLaunchKernel arg pack — order matches the kernel
         * signature: (ref, dist, partials, width, height, nbx, nby, plane, bpc). */
        void *args[] = {
            (void *)&s->d_ref[p], (void *)&s->d_dist[p], (void *)&s->d_partials[p],
            (void *)&width,       (void *)&height,       (void *)&nbx,
            (void *)&nby,         (void *)&plane_arg,    (void *)&bpc_arg,
        };
        rc = hipModuleLaunchKernel(s->func_psnr_hvs, nbx, nby, 1, PSNR_HVS_BLOCK_DIM,
                                   PSNR_HVS_BLOCK_DIM, 1, 0, str, args, NULL);
        if (rc != hipSuccess)
            return psnr_hvs_hip_rc(rc);

        /* D2H: partial sums. */
        rc = hipMemcpyAsync(s->h_partials[p], s->d_partials[p], partials_bytes,
                            hipMemcpyDeviceToHost, str);
        if (rc != hipSuccess)
            return psnr_hvs_hip_rc(rc);
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
    /* CPU-side normalise all planes for ref and dist. */
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
        upload_plane(s, ref_pic, p);
        upload_plane_dist(s, dist_pic, p);
    }

    int err = launch_psnr_hvs(s);
    if (err != 0)
        return err;

    /* Record the submit event on the kernel stream (no separate upload
     * stream here; matches the simpler single-stream HIP posture used by
     * float_psnr_hip). vmaf_hip_kernel_submit_post_record records the
     * finished event so collect() can wait for it. */
    hipError_t rc = hipEventRecord((hipEvent_t)s->lc.submit, (hipStream_t)s->lc.str);
    if (rc != hipSuccess)
        return psnr_hvs_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
#else
    (void)ref_pic;
    (void)dist_pic;
    int err = vmaf_hip_kernel_submit_pre_launch(&s->lc, s->ctx, NULL,
                                                /* picture_stream */ 0,
                                                /* dist_ready_event */ 0);
    if (err != 0)
        return err;
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
        if (s->h_ref[p]) {
            (void)hipHostFree(s->h_ref[p]);
            s->h_ref[p] = NULL;
        }
        if (s->h_dist[p]) {
            (void)hipHostFree(s->h_dist[p]);
            s->h_dist[p] = NULL;
        }
        if (s->h_partials[p]) {
            (void)hipHostFree(s->h_partials[p]);
            s->h_partials[p] = NULL;
        }
        if (s->h_uint_ref[p]) {
            (void)hipHostFree(s->h_uint_ref[p]);
            s->h_uint_ref[p] = NULL;
        }
        if (s->h_uint_dist[p]) {
            (void)hipHostFree(s->h_uint_dist[p]);
            s->h_uint_dist[p] = NULL;
        }
    }
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
    .flags = 0,
    .chars =
        {
            .n_dispatches_per_frame = 3,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
