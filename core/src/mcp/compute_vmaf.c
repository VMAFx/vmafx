/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  `compute_vmaf` MCP tool — real scoring binding for v2.
 *
 *  v1 (PR #490) shipped a `{"status":"deferred_to_v2"}` placeholder.
 *  v2 binds the existing libvmaf scoring API: a fresh VmafContext is
 *  initialised per call, the requested model is loaded, the YUV pair
 *  is read frame by frame via plain POSIX I/O, and the pooled mean
 *  VMAF score is returned.
 *
 *  The MCP server's borrowed VmafContext (`server->ctx`) is NOT
 *  reused — score-pooling is destructive (it commits the model to
 *  the context), so a per-call ephemeral context preserves the
 *  contract that the host's main scoring run is unaffected by an
 *  out-of-band MCP measurement.
 *
 *  Power-of-10 invariants:
 *      - rule 2: every loop is bounded — frame count is bounded by
 *        the YUV file size (computed up-front), the frame index
 *        bound is enforced by VMAF_MCP_COMPUTE_MAX_FRAMES.
 *      - rule 3: per-frame pictures use libvmaf's own
 *        `vmaf_picture_alloc` (heap, but bounded by frame count
 *        and freed via `vmaf_picture_unref` before the next read).
 *        The MCP measurement thread is not on the host's hot
 *        path; this is a one-shot tool call.
 *      - rule 7: every read()/vmaf_*() return value is checked.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "3rdparty/cJSON/cJSON.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "mcp_internal.h"

/* Hard cap on frames a single MCP call will score. The Netflix
 * golden corpus is 48 frames; production hosts that need more
 * should use the libvmaf CLI, not the MCP tool surface. */
#define VMAF_MCP_COMPUTE_MAX_FRAMES 4096u

/* Picture dimension bounds (per side). Floor is 4:2:0-even-friendly
 * and matches the minimum a VMAF feature window can address; the
 * 8192 ceiling is well beyond 8K UHD (7680 wide) and gives a safe
 * uint64 product for the bytes-per-frame computation. */
#define VMAF_MCP_PIC_DIM_MIN 8.0
#define VMAF_MCP_PIC_DIM_MAX 8192.0

/* Allowed pixel formats — the v2 surface accepts planar 4:2:0 with
 * 8/10/12/16-bit samples. Other chroma layouts remain -EINVAL until
 * the tool schema grows an explicit pixel_format field. */
#define VMAF_MCP_BITDEPTH_8 8u
#define VMAF_MCP_BITDEPTH_10 10u
#define VMAF_MCP_BITDEPTH_12 12u
#define VMAF_MCP_BITDEPTH_16 16u

/* Set *err_owned to a heap-allocated copy of `msg`. Returns 0 on
 * success, -ENOMEM if allocation fails. */
static int set_err(char **err_owned, const char *msg)
{
    size_t len = strlen(msg);
    char *dup = (char *)malloc(len + 1u);
    if (dup == NULL)
        return -ENOMEM;
    memcpy(dup, msg, len + 1u);
    *err_owned = dup;
    return 0;
}

/* Read exactly `want` bytes into buf. Returns 0 on success, -EIO on
 * short read or stream error. Treats partial-read-at-EOF as EIO so
 * the caller sees a single error code. */
static int read_exact(int fd, void *buf, size_t want)
{
    size_t got = 0u;
    while (got < want) {
        ssize_t r = read(fd, (char *)buf + got, want - got);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r == 0)
            return -EIO; /* short read at EOF. */
        if (errno == EINTR)
            continue;
        return -EIO;
    }
    return 0;
}

static size_t sample_bytes_for_bpc(unsigned bpc)
{
    return bpc > VMAF_MCP_BITDEPTH_8 ? 2u : 1u;
}

/* Fill `pic` with the next frame from `fd`. Plane sizes for 4:2:0:
 * Y = w*h, U = V = (w/2)*(h/2). High-bit-depth samples are read as
 * little-endian 16-bit payloads into libvmaf's native 16-bit picture
 * storage; the host is little-endian in all supported CI/runtime lanes. */
static int read_yuv420p_frame(int fd, VmafPicture *pic)
{
    const size_t sample_bytes = sample_bytes_for_bpc(pic->bpc);
    /* Y plane. */
    for (unsigned y = 0u; y < pic->h[0]; ++y) {
        unsigned char *row = (unsigned char *)pic->data[0] + (ptrdiff_t)y * pic->stride[0];
        int rc = read_exact(fd, row, (size_t)pic->w[0] * sample_bytes);
        if (rc != 0)
            return rc;
    }
    /* U + V planes. */
    for (unsigned p = 1u; p < 3u; ++p) {
        for (unsigned y = 0u; y < pic->h[p]; ++y) {
            unsigned char *row = (unsigned char *)pic->data[p] + (ptrdiff_t)y * pic->stride[p];
            int rc = read_exact(fd, row, (size_t)pic->w[p] * sample_bytes);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

/* File-size in bytes via stat; returns -EIO on failure. */
static int file_size(const char *path, uint64_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -EIO;
    if (st.st_size < 0)
        return -EIO;
    *out = (uint64_t)st.st_size;
    return 0;
}

typedef struct ComputeArgs {
    const char *reference_path;
    const char *distorted_path;
    unsigned width;
    unsigned height;
    unsigned bitdepth;
    const char *model_version; /* e.g. "vmaf_v0.6.1" */
} ComputeArgs;

/* Pull required + optional fields out of the JSON `arguments`
 * object. Returns 0 on success or sets *err_owned + returns
 * -EINVAL. */
static int parse_arguments(const cJSON *arguments, ComputeArgs *out, char **err_owned)
{
    if (arguments == NULL || !cJSON_IsObject(arguments))
        return set_err(err_owned, "compute_vmaf requires an object 'arguments' value") == 0 ?
                   -EINVAL :
                   -ENOMEM;

    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(arguments, "reference_path");
    const cJSON *dis = cJSON_GetObjectItemCaseSensitive(arguments, "distorted_path");
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(arguments, "width");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(arguments, "height");
    const cJSON *bd = cJSON_GetObjectItemCaseSensitive(arguments, "bitdepth");
    const cJSON *mv = cJSON_GetObjectItemCaseSensitive(arguments, "model_version");

    if (!cJSON_IsString(ref) || !cJSON_IsString(dis))
        return set_err(err_owned, "compute_vmaf requires string fields 'reference_path' "
                                  "and 'distorted_path'") == 0 ?
                   -EINVAL :
                   -ENOMEM;
    if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h))
        return set_err(err_owned, "compute_vmaf requires positive integer fields "
                                  "'width' and 'height' (YUV420p)") == 0 ?
                   -EINVAL :
                   -ENOMEM;
    double wv = w->valuedouble;
    double hv = h->valuedouble;
    if (wv < VMAF_MCP_PIC_DIM_MIN || hv < VMAF_MCP_PIC_DIM_MIN || wv > VMAF_MCP_PIC_DIM_MAX ||
        hv > VMAF_MCP_PIC_DIM_MAX)
        return set_err(err_owned, "width/height out of range [8, 8192]") == 0 ? -EINVAL : -ENOMEM;
    /* Even-dim required for 4:2:0. */
    if (((unsigned)wv & 1u) || ((unsigned)hv & 1u))
        return set_err(err_owned, "width and height must be even (YUV420p)") == 0 ? -EINVAL :
                                                                                    -ENOMEM;

    out->reference_path = ref->valuestring;
    out->distorted_path = dis->valuestring;
    out->width = (unsigned)wv;
    out->height = (unsigned)hv;
    out->bitdepth = VMAF_MCP_BITDEPTH_8;
    if (bd != NULL) {
        if (!cJSON_IsNumber(bd))
            return set_err(err_owned, "bitdepth must be one of 8, 10, 12, 16") == 0 ? -EINVAL :
                                                                                      -ENOMEM;
        double bdv = bd->valuedouble;
        out->bitdepth = (unsigned)bdv;
    }
    if (out->bitdepth != VMAF_MCP_BITDEPTH_8 && out->bitdepth != VMAF_MCP_BITDEPTH_10 &&
        out->bitdepth != VMAF_MCP_BITDEPTH_12 && out->bitdepth != VMAF_MCP_BITDEPTH_16)
        return set_err(err_owned, "bitdepth must be one of 8, 10, 12, 16") == 0 ? -EINVAL : -ENOMEM;
    out->model_version = (cJSON_IsString(mv) ? mv->valuestring : "vmaf_v0.6.1");
    return 0;
}

/* Score the YUV pair end to end. Returns 0 + sets *score_out on
 * success; sets *err_owned + returns negative errno on failure. */
static int score_yuv_pair(const ComputeArgs *args, double *score_out, unsigned *frames_scored_out,
                          char **err_owned)
{
    int rc = 0;
    int rfd = -1;
    int dfd = -1;
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;

    /* Per-frame YUV420p byte count. High-bit-depth content is stored
     * as 16-bit little-endian samples. */
    const uint64_t bytes_per_sample = (uint64_t)sample_bytes_for_bpc(args->bitdepth);
    const uint64_t bytes_per_frame =
        (uint64_t)args->width * (uint64_t)args->height * 3u / 2u * bytes_per_sample;
    if (bytes_per_frame == 0u) {
        rc = set_err(err_owned, "computed frame size is zero") == 0 ? -EINVAL : -ENOMEM;
        goto cleanup;
    }

    uint64_t ref_size = 0u;
    uint64_t dis_size = 0u;
    if (file_size(args->reference_path, &ref_size) != 0) {
        rc = set_err(err_owned, "cannot stat reference_path") == 0 ? -EIO : -ENOMEM;
        goto cleanup;
    }
    if (file_size(args->distorted_path, &dis_size) != 0) {
        rc = set_err(err_owned, "cannot stat distorted_path") == 0 ? -EIO : -ENOMEM;
        goto cleanup;
    }
    if (ref_size != dis_size) {
        rc =
            set_err(err_owned, "reference and distorted YUV sizes differ") == 0 ? -EINVAL : -ENOMEM;
        goto cleanup;
    }
    if (ref_size % bytes_per_frame != 0u) {
        rc = set_err(err_owned, "YUV file size is not a multiple of "
                                "frame size — width/height likely wrong") == 0 ?
                 -EINVAL :
                 -ENOMEM;
        goto cleanup;
    }
    uint64_t frame_count = ref_size / bytes_per_frame;
    if (frame_count == 0u || frame_count > VMAF_MCP_COMPUTE_MAX_FRAMES) {
        rc = set_err(err_owned, "frame count out of range [1, 4096]") == 0 ? -EINVAL : -ENOMEM;
        goto cleanup;
    }

    rfd = open(args->reference_path, O_RDONLY);
    if (rfd < 0) {
        rc = set_err(err_owned, "open(reference_path) failed") == 0 ? -EIO : -ENOMEM;
        goto cleanup;
    }
    dfd = open(args->distorted_path, O_RDONLY);
    if (dfd < 0) {
        rc = set_err(err_owned, "open(distorted_path) failed") == 0 ? -EIO : -ENOMEM;
        goto cleanup;
    }

    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    int vrc = vmaf_init(&vmaf, vcfg);
    if (vrc != 0) {
        rc = set_err(err_owned, "vmaf_init failed") == 0 ? vrc : -ENOMEM;
        goto cleanup;
    }

    VmafModelConfig mcfg = {0};
    mcfg.name = "vmaf";
    int mrc = vmaf_model_load(&model, &mcfg, args->model_version);
    if (mrc != 0) {
        rc = set_err(err_owned, "vmaf_model_load failed (unknown model_version?)") == 0 ? mrc :
                                                                                          -ENOMEM;
        goto cleanup;
    }
    int urc = vmaf_use_features_from_model(vmaf, model);
    if (urc != 0) {
        rc = set_err(err_owned, "vmaf_use_features_from_model failed") == 0 ? urc : -ENOMEM;
        goto cleanup;
    }

    unsigned frames_scored = 0u;
    for (uint64_t i = 0u; i < frame_count && i < VMAF_MCP_COMPUTE_MAX_FRAMES; ++i) {
        VmafPicture rpic = {0};
        VmafPicture dpic = {0};
        int prc = vmaf_picture_alloc(&rpic, VMAF_PIX_FMT_YUV420P, args->bitdepth, args->width,
                                     args->height);
        if (prc != 0) {
            rc = set_err(err_owned, "vmaf_picture_alloc(ref) failed") == 0 ? prc : -ENOMEM;
            goto cleanup;
        }
        int qrc = vmaf_picture_alloc(&dpic, VMAF_PIX_FMT_YUV420P, args->bitdepth, args->width,
                                     args->height);
        if (qrc != 0) {
            (void)vmaf_picture_unref(&rpic);
            rc = set_err(err_owned, "vmaf_picture_alloc(dis) failed") == 0 ? qrc : -ENOMEM;
            goto cleanup;
        }
        int rrc = read_yuv420p_frame(rfd, &rpic);
        int drc = read_yuv420p_frame(dfd, &dpic);
        if (rrc != 0 || drc != 0) {
            (void)vmaf_picture_unref(&rpic);
            (void)vmaf_picture_unref(&dpic);
            rc = set_err(err_owned, "YUV frame read failed") == 0 ? -EIO : -ENOMEM;
            goto cleanup;
        }
        int read_rc = vmaf_read_pictures(vmaf, &rpic, &dpic, (unsigned)i);
        if (read_rc != 0) {
            rc = set_err(err_owned, "vmaf_read_pictures failed") == 0 ? read_rc : -ENOMEM;
            goto cleanup;
        }
        frames_scored++;
    }
    /* Flush — signal end-of-stream. */
    int flush_rc = vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    if (flush_rc != 0) {
        rc = set_err(err_owned, "vmaf_read_pictures(flush) failed") == 0 ? flush_rc : -ENOMEM;
        goto cleanup;
    }
    double pooled = 0.0;
    int srcc = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &pooled, 0u,
                                 frames_scored > 0u ? frames_scored - 1u : 0u);
    if (srcc != 0) {
        rc = set_err(err_owned, "vmaf_score_pooled failed") == 0 ? srcc : -ENOMEM;
        goto cleanup;
    }
    *score_out = pooled;
    *frames_scored_out = frames_scored;

cleanup:
    if (model != NULL)
        vmaf_model_destroy(model);
    if (vmaf != NULL)
        (void)vmaf_close(vmaf);
    if (rfd >= 0)
        (void)close(rfd);
    if (dfd >= 0)
        (void)close(dfd);
    return rc;
}

int vmaf_mcp_compute_vmaf(const void *arguments_cjson, void **result_out_cjson, char **err_owned)
{
    assert(arguments_cjson != NULL);
    assert(result_out_cjson != NULL);
    const cJSON *arguments = (const cJSON *)arguments_cjson;
    ComputeArgs args = {0};
    int rc = parse_arguments(arguments, &args, err_owned);
    if (rc != 0)
        return rc;

    double score = 0.0;
    unsigned frames = 0u;
    int srcc = score_yuv_pair(&args, &score, &frames, err_owned);
    if (srcc != 0)
        return srcc;
    assert(frames > 0u);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL)
        return -ENOMEM;
    cJSON_AddNumberToObject(result, "score", score);
    cJSON_AddNumberToObject(result, "frames_scored", (double)frames);
    cJSON_AddStringToObject(result, "model_version", args.model_version);
    cJSON_AddNumberToObject(result, "bitdepth", (double)args.bitdepth);
    cJSON_AddStringToObject(result, "reference_path", args.reference_path);
    cJSON_AddStringToObject(result, "distorted_path", args.distorted_path);
    cJSON_AddStringToObject(result, "pool_method", "mean");
    *result_out_cjson = result;
    return 0;
}
