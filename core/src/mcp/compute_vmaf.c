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
#include <limits.h>
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

/* ----------------------------------------------------------------------
 * Path allowlist — mirrors the Python (mcp-server/vmaf-mcp/src/vmaf_mcp/
 * server.py:_allowed_roots / _validate_path) and Go (pkg/libvmaf/paths.go
 * AllowedRoots / ValidatePath) policies EXACTLY. A caller-supplied YUV path
 * must canonicalise (realpath(3) — resolves symlinks and `../` traversal)
 * to a location under one of the allowlisted roots, otherwise the request is
 * rejected before any stat()/open(). Without this guard the C MCP scoring
 * binding read arbitrary filesystem paths (R2-4).
 *
 * Default roots (identical set across all three servers):
 *   - <repo>/testdata
 *   - <repo>/python/test/resource
 *   - <repo>/model
 *   - /workspace/python/test/resource   (vmaf-dev-mcp container mount)
 * plus any colon-separated absolute paths in $VMAF_MCP_ALLOW.
 * -------------------------------------------------------------------- */

/* Maximum number of allowlisted roots: 4 defaults + headroom for
 * VMAF_MCP_ALLOW extras. Power-of-10 rule 2 (bounded). */
#define VMAF_MCP_MAX_ALLOW_ROOTS 32u

/* Relative repo subtrees that are always allowlisted (joined to the
 * discovered repo root). Mirrors _allowed_roots() / AllowedRoots(). */
static const char *const k_default_rel_roots[] = {
    "testdata",
    "python/test/resource",
    "model",
};

/* Absolute roots that are always allowlisted regardless of repo root —
 * the container bind-mount path (ADR-0496). */
static const char *const k_default_abs_roots[] = {
    "/workspace/python/test/resource",
};

/* Locate the repository root by walking up from the current working
 * directory looking for the CLAUDE.md marker, mirroring Go's RepoRoot().
 * Writes the canonical (realpath) root into `out` (size `out_sz`).
 * Returns 0 on success, -1 if no marker is found or the buffer is too
 * small. */
static int discover_repo_root(char *out, size_t out_sz)
{
    char dir[PATH_MAX];
    if (getcwd(dir, sizeof(dir)) == NULL)
        return -1;

    /* Bounded walk: PATH_MAX/2 components is a hard ceiling on directory
     * depth, well beyond any real filesystem (Power-of-10 rule 2). */
    for (unsigned depth = 0u; depth < (unsigned)(PATH_MAX / 2); ++depth) {
        char marker[PATH_MAX];
        int n = snprintf(marker, sizeof(marker), "%s/CLAUDE.md", dir);
        if (n < 0 || (size_t)n >= sizeof(marker))
            return -1;
        struct stat st;
        if (stat(marker, &st) == 0 && S_ISREG(st.st_mode)) {
            char real[PATH_MAX];
            if (realpath(dir, real) == NULL)
                return -1;
            size_t len = strlen(real);
            if (len + 1u > out_sz)
                return -1;
            memcpy(out, real, len + 1u);
            return 0;
        }
        /* Ascend one level. */
        char *slash = strrchr(dir, '/');
        if (slash == NULL || slash == dir) {
            /* Reached "/" or a relative single component — no marker. */
            return -1;
        }
        *slash = '\0';
    }
    return -1;
}

/* Canonicalise `candidate` with realpath(3) and append the result to
 * roots[*count] if it resolves and fits. Roots that do not exist on disk
 * are silently skipped (realpath fails), matching the Python/Go behaviour
 * where a missing root simply never matches. Bounded by `cap`. */
static void append_canonical_root(const char *candidate, char roots[][PATH_MAX], unsigned *count,
                                  unsigned cap)
{
    if (*count >= cap)
        return;
    char real[PATH_MAX];
    if (realpath(candidate, real) == NULL)
        return;
    size_t len = strlen(real);
    if (len + 1u > PATH_MAX)
        return;
    memcpy(roots[*count], real, len + 1u);
    ++(*count);
}

/* Append the colon-separated absolute paths from $VMAF_MCP_ALLOW (mirrors
 * the Python `extra.split(":")` / Go filepath.SplitList behaviour). */
static void append_env_roots(char roots[][PATH_MAX], unsigned *count, unsigned cap)
{
    /* The MCP compute call resolves the env once per request and the
     * ADR-0461 caller-contract bans concurrent setenv on the MCP path, so
     * the concurrency-mt-unsafe getenv is safe here (same posture as
     * gpu_dispatch_env). */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-0461 caller-contract. */
    const char *extra = getenv("VMAF_MCP_ALLOW");
    if (extra == NULL || extra[0] == '\0')
        return;
    char buf[PATH_MAX * 4];
    size_t elen = strlen(extra);
    if (elen + 1u > sizeof(buf))
        return;
    memcpy(buf, extra, elen + 1u);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ":", &save); tok != NULL && *count < cap;
         tok = strtok_r(NULL, ":", &save)) {
        if (tok[0] != '\0')
            append_canonical_root(tok, roots, count, cap);
    }
}

/* Build the canonical allowlist root set into `roots` (each entry up to
 * PATH_MAX). Returns the number of roots written (0 on total failure). */
static unsigned build_allowed_roots(char roots[][PATH_MAX], unsigned cap)
{
    unsigned count = 0u;
    char repo_root[PATH_MAX];
    if (discover_repo_root(repo_root, sizeof(repo_root)) == 0) {
        const size_t n_rel = sizeof(k_default_rel_roots) / sizeof(k_default_rel_roots[0]);
        for (size_t i = 0u; i < n_rel && count < cap; ++i) {
            char joined[PATH_MAX];
            int n = snprintf(joined, sizeof(joined), "%s/%s", repo_root, k_default_rel_roots[i]);
            if (n > 0 && (size_t)n < sizeof(joined))
                append_canonical_root(joined, roots, &count, cap);
        }
    }

    const size_t n_abs = sizeof(k_default_abs_roots) / sizeof(k_default_abs_roots[0]);
    for (size_t i = 0u; i < n_abs && count < cap; ++i)
        append_canonical_root(k_default_abs_roots[i], roots, &count, cap);

    append_env_roots(roots, &count, cap);
    return count;
}

/* Return non-zero iff `path` is lexically under `root` as a path-component
 * prefix (so "/a/bc" is NOT under "/a/b"). Both inputs are canonical
 * (realpath output). Mirrors the trailing-separator prefix test used by
 * Go ValidatePath and Python Path.is_relative_to. */
static int path_under_root(const char *path, const char *root)
{
    size_t rlen = strlen(root);
    if (rlen == 0u)
        return 0;
    /* Allow the path to equal the root exactly, or sit beneath it with a
     * '/' boundary. A root of "/" matches everything beneath it. */
    if (strncmp(path, root, rlen) != 0)
        return 0;
    if (path[rlen] == '\0')
        return 1; /* path == root */
    if (rlen == 1u && root[0] == '/')
        return 1; /* root is "/" */
    return path[rlen] == '/';
}

/* Canonicalise `in_path` with realpath(3) and require the result to sit
 * under one of the allowlisted roots. On success writes the canonical
 * path into `out` (size `out_sz`) and returns 0. On violation sets
 * *err_owned (caller frees) and returns -EACCES; on realpath/ENOMEM
 * failure returns the corresponding negative errno. */
static int validate_path(const char *in_path, char *out, size_t out_sz, char **err_owned)
{
    if (in_path == NULL || in_path[0] == '\0') {
        return set_err(err_owned, "empty path") == 0 ? -EINVAL : -ENOMEM;
    }
    /* realpath() resolves symlinks AND `..`/`.` traversal, and requires the
     * target to exist. A non-existent or unreadable path fails here, before
     * any open(). */
    char *resolved = realpath(in_path, NULL);
    if (resolved == NULL) {
        return set_err(err_owned, "path does not resolve to an existing file") == 0 ? -ENOENT :
                                                                                      -ENOMEM;
    }

    char roots[VMAF_MCP_MAX_ALLOW_ROOTS][PATH_MAX];
    unsigned n_roots = build_allowed_roots(roots, VMAF_MCP_MAX_ALLOW_ROOTS);

    int allowed = 0;
    for (unsigned i = 0u; i < n_roots; ++i) {
        if (path_under_root(resolved, roots[i])) {
            allowed = 1;
            break;
        }
    }
    if (!allowed) {
        free(resolved);
        return set_err(err_owned, "path is not under an allowlisted root; "
                                  "set VMAF_MCP_ALLOW to extend") == 0 ?
                   -EACCES :
                   -ENOMEM;
    }

    size_t len = strlen(resolved);
    if (len + 1u > out_sz) {
        free(resolved);
        return set_err(err_owned, "resolved path too long") == 0 ? -ENAMETOOLONG : -ENOMEM;
    }
    memcpy(out, resolved, len + 1u);
    free(resolved);
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
    if (arguments == NULL || !cJSON_IsObject(arguments)) {
        return set_err(err_owned, "compute_vmaf requires an object 'arguments' value") == 0 ?
                   -EINVAL :
                   -ENOMEM;
    }

    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(arguments, "reference_path");
    const cJSON *dis = cJSON_GetObjectItemCaseSensitive(arguments, "distorted_path");
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(arguments, "width");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(arguments, "height");
    const cJSON *bd = cJSON_GetObjectItemCaseSensitive(arguments, "bitdepth");
    const cJSON *mv = cJSON_GetObjectItemCaseSensitive(arguments, "model_version");

    if (!cJSON_IsString(ref) || !cJSON_IsString(dis)) {
        return set_err(err_owned, "compute_vmaf requires string fields 'reference_path' "
                                  "and 'distorted_path'") == 0 ?
                   -EINVAL :
                   -ENOMEM;
    }
    if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h)) {
        return set_err(err_owned, "compute_vmaf requires positive integer fields "
                                  "'width' and 'height' (YUV420p)") == 0 ?
                   -EINVAL :
                   -ENOMEM;
    }
    double wv = w->valuedouble;
    double hv = h->valuedouble;
    if (wv < VMAF_MCP_PIC_DIM_MIN || hv < VMAF_MCP_PIC_DIM_MIN || wv > VMAF_MCP_PIC_DIM_MAX ||
        hv > VMAF_MCP_PIC_DIM_MAX) {
        return set_err(err_owned, "width/height out of range [8, 8192]") == 0 ? -EINVAL : -ENOMEM;
    }
    /* Even-dim required for 4:2:0. */
    if (((unsigned)wv & 1u) || ((unsigned)hv & 1u)) {
        return set_err(err_owned, "width and height must be even (YUV420p)") == 0 ? -EINVAL :
                                                                                    -ENOMEM;
    }

    out->reference_path = ref->valuestring;
    out->distorted_path = dis->valuestring;
    out->width = (unsigned)wv;
    out->height = (unsigned)hv;
    out->bitdepth = VMAF_MCP_BITDEPTH_8;
    if (bd != NULL) {
        if (!cJSON_IsNumber(bd)) {
            return set_err(err_owned, "bitdepth must be one of 8, 10, 12, 16") == 0 ? -EINVAL :
                                                                                      -ENOMEM;
        }
        double bdv = bd->valuedouble;
        out->bitdepth = (unsigned)bdv;
    }
    if (out->bitdepth != VMAF_MCP_BITDEPTH_8 && out->bitdepth != VMAF_MCP_BITDEPTH_10 &&
        out->bitdepth != VMAF_MCP_BITDEPTH_12 && out->bitdepth != VMAF_MCP_BITDEPTH_16) {
        return set_err(err_owned, "bitdepth must be one of 8, 10, 12, 16") == 0 ? -EINVAL : -ENOMEM;
    }
    out->model_version = (cJSON_IsString(mv) ? mv->valuestring : "vmaf_v0.6.1");
    return 0;
}

/* Validate the two (already canonical) YUV paths' byte geometry and derive
 * the frame count. Returns 0 + sets *frame_count_out; on failure sets
 * *err_owned and returns negative errno. */
static int derive_frame_count(const ComputeArgs *args, const char *ref_real, const char *dis_real,
                              uint64_t *frame_count_out, char **err_owned)
{
    /* Per-frame YUV420p byte count. High-bit-depth content is stored as
     * 16-bit little-endian samples. */
    const uint64_t bytes_per_sample = (uint64_t)sample_bytes_for_bpc(args->bitdepth);
    const uint64_t bytes_per_frame =
        (uint64_t)args->width * (uint64_t)args->height * 3u / 2u * bytes_per_sample;
    if (bytes_per_frame == 0u)
        return set_err(err_owned, "computed frame size is zero") == 0 ? -EINVAL : -ENOMEM;

    uint64_t ref_size = 0u;
    uint64_t dis_size = 0u;
    if (file_size(ref_real, &ref_size) != 0)
        return set_err(err_owned, "cannot stat reference_path") == 0 ? -EIO : -ENOMEM;
    if (file_size(dis_real, &dis_size) != 0)
        return set_err(err_owned, "cannot stat distorted_path") == 0 ? -EIO : -ENOMEM;
    if (ref_size != dis_size) {
        return set_err(err_owned, "reference and distorted YUV sizes differ") == 0 ? -EINVAL :
                                                                                     -ENOMEM;
    }
    if (ref_size % bytes_per_frame != 0u) {
        return set_err(err_owned, "YUV file size is not a multiple of "
                                  "frame size — width/height likely wrong") == 0 ?
                   -EINVAL :
                   -ENOMEM;
    }
    uint64_t frame_count = ref_size / bytes_per_frame;
    if (frame_count == 0u || frame_count > VMAF_MCP_COMPUTE_MAX_FRAMES)
        return set_err(err_owned, "frame count out of range [1, 4096]") == 0 ? -EINVAL : -ENOMEM;
    *frame_count_out = frame_count;
    return 0;
}

/* Read + submit `frame_count` frame pairs into `vmaf`. Returns 0 + sets
 * *frames_scored_out; on failure sets *err_owned and returns negative errno. */
static int feed_frames(VmafContext *vmaf, const ComputeArgs *args, int rfd, int dfd,
                       uint64_t frame_count, unsigned *frames_scored_out, char **err_owned)
{
    unsigned frames_scored = 0u;
    for (uint64_t i = 0u; i < frame_count && i < VMAF_MCP_COMPUTE_MAX_FRAMES; ++i) {
        VmafPicture rpic = {0};
        VmafPicture dpic = {0};
        int prc = vmaf_picture_alloc(&rpic, VMAF_PIX_FMT_YUV420P, args->bitdepth, args->width,
                                     args->height);
        if (prc != 0)
            return set_err(err_owned, "vmaf_picture_alloc(ref) failed") == 0 ? prc : -ENOMEM;
        int qrc = vmaf_picture_alloc(&dpic, VMAF_PIX_FMT_YUV420P, args->bitdepth, args->width,
                                     args->height);
        if (qrc != 0) {
            (void)vmaf_picture_unref(&rpic);
            return set_err(err_owned, "vmaf_picture_alloc(dis) failed") == 0 ? qrc : -ENOMEM;
        }
        int rrc = read_yuv420p_frame(rfd, &rpic);
        int drc = read_yuv420p_frame(dfd, &dpic);
        if (rrc != 0 || drc != 0) {
            (void)vmaf_picture_unref(&rpic);
            (void)vmaf_picture_unref(&dpic);
            return set_err(err_owned, "YUV frame read failed") == 0 ? -EIO : -ENOMEM;
        }
        int read_rc = vmaf_read_pictures(vmaf, &rpic, &dpic, (unsigned)i);
        if (read_rc != 0)
            return set_err(err_owned, "vmaf_read_pictures failed") == 0 ? read_rc : -ENOMEM;
        frames_scored++;
    }
    *frames_scored_out = frames_scored;
    return 0;
}

/* Create an ephemeral single-threaded VmafContext and load the requested
 * model, mounting its features. On success sets *vmaf_out + *model_out (both
 * owned by the caller); on failure sets *err_owned and returns negative
 * errno (leaving any partial allocation for the caller's cleanup path). */
static int init_vmaf_and_model(const ComputeArgs *args, VmafContext **vmaf_out,
                               VmafModel **model_out, char **err_owned)
{
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    int vrc = vmaf_init(vmaf_out, vcfg);
    if (vrc != 0)
        return set_err(err_owned, "vmaf_init failed") == 0 ? vrc : -ENOMEM;

    VmafModelConfig mcfg = {0};
    mcfg.name = "vmaf";
    int mrc = vmaf_model_load(model_out, &mcfg, args->model_version);
    if (mrc != 0) {
        return set_err(err_owned, "vmaf_model_load failed (unknown model_version?)") == 0 ? mrc :
                                                                                            -ENOMEM;
    }
    int urc = vmaf_use_features_from_model(*vmaf_out, *model_out);
    if (urc != 0)
        return set_err(err_owned, "vmaf_use_features_from_model failed") == 0 ? urc : -ENOMEM;
    return 0;
}

/* Flush the end-of-stream sentinel and pool the mean VMAF over the
 * frames submitted. Returns 0 + sets *score_out; on failure sets
 * *err_owned and returns negative errno. */
static int flush_and_pool(VmafContext *vmaf, VmafModel *model, unsigned frames_scored,
                          double *score_out, char **err_owned)
{
    /* Flush — signal end-of-stream. */
    int flush_rc = vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    if (flush_rc != 0)
        return set_err(err_owned, "vmaf_read_pictures(flush) failed") == 0 ? flush_rc : -ENOMEM;
    double pooled = 0.0;
    int srcc = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &pooled, 0u,
                                 frames_scored > 0u ? frames_scored - 1u : 0u);
    if (srcc != 0)
        return set_err(err_owned, "vmaf_score_pooled failed") == 0 ? srcc : -ENOMEM;
    *score_out = pooled;
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

    /* SECURITY (R2-4): canonicalise + allowlist-check both caller paths
     * BEFORE any stat()/open(). Mirrors the Python/Go MCP servers. Use the
     * resolved canonical paths for every subsequent filesystem operation. */
    char ref_real[PATH_MAX];
    char dis_real[PATH_MAX];
    rc = validate_path(args->reference_path, ref_real, sizeof(ref_real), err_owned);
    if (rc != 0)
        goto cleanup;
    rc = validate_path(args->distorted_path, dis_real, sizeof(dis_real), err_owned);
    if (rc != 0)
        goto cleanup;

    uint64_t frame_count = 0u;
    rc = derive_frame_count(args, ref_real, dis_real, &frame_count, err_owned);
    if (rc != 0)
        goto cleanup;

    rfd = open(ref_real, O_RDONLY);
    if (rfd < 0) {
        rc = set_err(err_owned, "open(reference_path) failed") == 0 ? -EIO : -ENOMEM;
        goto cleanup;
    }
    dfd = open(dis_real, O_RDONLY);
    if (dfd < 0) {
        rc = set_err(err_owned, "open(distorted_path) failed") == 0 ? -EIO : -ENOMEM;
        goto cleanup;
    }

    rc = init_vmaf_and_model(args, &vmaf, &model, err_owned);
    if (rc != 0)
        goto cleanup;

    unsigned frames_scored = 0u;
    rc = feed_frames(vmaf, args, rfd, dfd, frame_count, &frames_scored, err_owned);
    if (rc != 0)
        goto cleanup;

    rc = flush_and_pool(vmaf, model, frames_scored, score_out, err_owned);
    if (rc != 0)
        goto cleanup;
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
