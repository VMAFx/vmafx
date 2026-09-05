/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  vmaf-roi — sidecar binary that consumes a saliency map (per-frame) from
 *  a tiny ONNX model and emits a per-CTU QP-offset sidecar file consumable
 *  by x265 (--qpfile) and SVT-AV1 (--roi-map-file).
 *
 *  Pipeline:
 *      raw YUV (Y plane only, planar 4:2:0/4:2:2/4:4:4, 8/10/12/16 bit)
 *          -> luma8 -> ONNX saliency model (optional) -> [0, 1] saliency map
 *          -> per-CTU mean reduce
 *          -> linear map to signed QP offsets in [-12, +12]
 *          -> ASCII (x265) or binary int8 (SVT-AV1) sidecar
 *
 *  When --saliency-model is absent the tool falls back to a deterministic
 *  radial center-weighted placeholder. The placeholder lets the sidecar
 *  pipeline (decode -> reduce -> emit) be exercised by a smoke test even
 *  when no ONNX runtime / no MobileSal weights are available; it MUST NOT
 *  be used to drive a real encode.
 *
 *  T6-2b. ADR-0247.
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/dnn.h"
#include "compat/path_utf8.h"
#include "vmaf_roi_core.h"

/* x265 / SVT-AV1 ROI sidecars use a small integer offset range. We clamp
 * to +-12 which is comfortably inside both encoders' accepted bands. */
#define VMAF_ROI_QP_OFFSET_MAX VMAF_ROI_CORE_QP_OFFSET_MAX

/* Hard upper limits. Power-of-10 rule 2 (bounded loops): every dimension
 * is checked against these caps before any allocation. */
#define VMAF_ROI_MAX_DIM 16384
#define VMAF_ROI_MIN_CTU 8
#define VMAF_ROI_MAX_CTU 128
#define VMAF_ROI_MAX_FRAME_INDEX 1000000

enum vmaf_roi_encoder {
    VMAF_ROI_ENCODER_X265 = 0,
    VMAF_ROI_ENCODER_SVTAV1 = 1,
};

enum vmaf_roi_pixfmt {
    VMAF_ROI_PIXFMT_420 = 0,
    VMAF_ROI_PIXFMT_422 = 1,
    VMAF_ROI_PIXFMT_444 = 2,
};

struct vmaf_roi_opts {
    const char *reference;      /* path to raw YUV input */
    const char *output;         /* path to sidecar (- = stdout) */
    const char *saliency_model; /* optional ONNX path */
    int width;
    int height;
    int frame;    /* 0-based frame index to score */
    int ctu_size; /* in luma samples */
    int bitdepth; /* 8-bit samples or 16-bit little-endian containers */
    enum vmaf_roi_pixfmt pixfmt;
    enum vmaf_roi_encoder encoder;
    double strength; /* QP-offset gain; saliency=1 -> -strength */
};

static void print_usage(FILE *out)
{
    /* fprintf with a literal format -> safe; no banned funcs touched. */
    (void)fprintf(out, "usage: vmaf-roi --reference REF.yuv --width W --height H \\\n"
                       "                --frame N --output qpfile [options]\n"
                       "\n"
                       "Required:\n"
                       "  --reference PATH         Raw planar YUV input.\n"
                       "  --width W                Frame width in luma samples.\n"
                       "  --height H               Frame height in luma samples.\n"
                       "  --frame N                0-based frame index to score.\n"
                       "  --output PATH            Sidecar file (use '-' for stdout).\n"
                       "\n"
                       "Optional:\n"
                       "  --pixel_format 420|422|444   Default 420.\n"
                       "  --bitdepth 8|10|12|16        Default 8.\n"
                       "  --ctu-size N                 8..128, default 64 (x265 max-ctu).\n"
                       "  --encoder x265|svt-av1       Default x265.\n"
                       "  --strength F                 QP-offset gain, default 6.0.\n"
                       "  --saliency-model PATH        Optional ONNX [1,1,H,W] luma->[0,1].\n"
                       "                               If absent, a center-weighted radial\n"
                       "                               placeholder is used (smoke-test only).\n"
                       "  -h, --help                   Show this help.\n");
}

static int parse_pixfmt(const char *arg, enum vmaf_roi_pixfmt *out)
{
    if (arg == NULL || out == NULL)
        return -EINVAL;
    if (strcmp(arg, "420") == 0) {
        *out = VMAF_ROI_PIXFMT_420;
        return 0;
    }
    if (strcmp(arg, "422") == 0) {
        *out = VMAF_ROI_PIXFMT_422;
        return 0;
    }
    if (strcmp(arg, "444") == 0) {
        *out = VMAF_ROI_PIXFMT_444;
        return 0;
    }
    return -EINVAL;
}

static int parse_encoder(const char *arg, enum vmaf_roi_encoder *out)
{
    if (arg == NULL || out == NULL)
        return -EINVAL;
    if (strcmp(arg, "x265") == 0) {
        *out = VMAF_ROI_ENCODER_X265;
        return 0;
    }
    if (strcmp(arg, "svt-av1") == 0 || strcmp(arg, "svtav1") == 0) {
        *out = VMAF_ROI_ENCODER_SVTAV1;
        return 0;
    }
    return -EINVAL;
}

/* strtol-based integer parse with explicit range check. The CLAUDE.md ban
 * list explicitly forbids atoi/atof. */
static int parse_int_arg(const char *arg, long lo, long hi, long *out)
{
    if (arg == NULL || out == NULL)
        return -EINVAL;
    char *end = NULL;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0')
        return -EINVAL;
    if (v < lo || v > hi)
        return -ERANGE;
    *out = v;
    return 0;
}

static int parse_double_arg(const char *arg, double lo, double hi, double *out)
{
    if (arg == NULL || out == NULL)
        return -EINVAL;
    char *end = NULL;
    errno = 0;
    double v = strtod(arg, &end);
    if (errno != 0 || end == arg || *end != '\0')
        return -EINVAL;
    if (v < lo || v > hi)
        return -ERANGE;
    *out = v;
    return 0;
}

static size_t luma_plane_size(int w, int h)
{
    /* Both bounded by VMAF_ROI_MAX_DIM, so the product fits in size_t. */
    return (size_t)w * (size_t)h;
}

static size_t sample_bytes_for_bitdepth(int bitdepth)
{
    return (bitdepth > 8) ? 2U : 1U;
}

/* Frame size in bytes for planar YUV, used to seek to the requested frame.
 * We ignore chroma content (saliency is luma-only) but we must still skip
 * past it in the file. High-bit-depth YUV is interpreted as little-endian
 * 16-bit containers, matching this fork's CLI conventions.
 *
 * Chroma plane dimensions use ceiling division because YUV subsampling
 * truncates to integer plane sizes regardless of luma parity:
 *   4:2:0  Cb/Cr: ceil(w/2) * ceil(h/2)  each
 *   4:2:2  Cb/Cr: ceil(w/2) * h          each
 *   4:4:4  Cb/Cr: w * h                  each
 *
 * Using y/2 (integer truncation) instead of 2*ceil(w/2)*ceil(h/2) would
 * under-count by up to one chroma row/column for odd-dimension inputs,
 * causing fseeko() to land at the wrong byte offset for frame indices > 0.
 */
static size_t frame_bytes(int w, int h, enum vmaf_roi_pixfmt pf, int bitdepth)
{
    const size_t y = luma_plane_size(w, h);
    /* Ceiling-divide width/height to get subsampled chroma plane dimensions. */
    const size_t cw = ((size_t)w + 1U) / 2U; /* ceil(w/2) */
    const size_t ch = ((size_t)h + 1U) / 2U; /* ceil(h/2) */
    size_t samples = 0U;
    switch (pf) {
    case VMAF_ROI_PIXFMT_420:
        samples = y + 2U * cw * ch; /* Y + Cb + Cr, each cw*ch */
        break;
    case VMAF_ROI_PIXFMT_422:
        samples = y + 2U * cw * (size_t)h; /* Y + Cb + Cr, each cw*h */
        break;
    case VMAF_ROI_PIXFMT_444:
        samples = y + 2U * y; /* Y + Cb + Cr, each w*h */
        break;
    }
    return samples * sample_bytes_for_bitdepth(bitdepth);
}

static int read_luma8(FILE *fp, uint8_t *dst, size_t y_sz, int bitdepth, size_t *got_bytes)
{
    if (got_bytes == NULL)
        return -EINVAL;
    *got_bytes = 0U;
    if (bitdepth == 8) {
        size_t got = fread(dst, 1U, y_sz, fp);
        *got_bytes = got;
        if (got != y_sz)
            return -EIO;
        return 0;
    }

    uint8_t *raw = (uint8_t *)malloc(y_sz * 2U);
    if (raw == NULL)
        return -ENOMEM;
    size_t got = fread(raw, 2U, y_sz, fp);
    *got_bytes = got * 2U;
    if (got != y_sz) {
        free(raw);
        return -EIO;
    }

    const unsigned shift = (unsigned)bitdepth - 8U;
    const unsigned round = (shift == 0U) ? 0U : (1U << (shift - 1U));
    const unsigned max_sample = (1U << (unsigned)bitdepth) - 1U;
    for (size_t i = 0U; i < y_sz; ++i) {
        unsigned v = (unsigned)raw[i * 2U] | ((unsigned)raw[i * 2U + 1U] << 8U);
        if (v > max_sample)
            v = max_sample;
        dst[i] = (uint8_t)((v + round) >> shift);
    }
    free(raw);
    return 0;
}

static int load_luma_frame(const struct vmaf_roi_opts *o, uint8_t *dst)
{
    FILE *fp = vmaf_fopen_utf8(o->reference, "rb");
    if (fp == NULL) {
        const int saved = errno;
        (void)fprintf(stderr, "vmaf-roi: cannot open %s: errno=%d\n", o->reference, saved);
        return -ENOENT;
    }

    const size_t frame_sz = frame_bytes(o->width, o->height, o->pixfmt, o->bitdepth);
    if (frame_sz == 0U) {
        (void)fclose(fp);
        return -EINVAL;
    }

#if defined(_WIN32)
    if (_fseeki64(fp, (long long)frame_sz * (long long)o->frame, SEEK_SET) != 0) {
#else
    /* fseeko + off_t for >2 GiB inputs */
    if (fseeko(fp, (off_t)((uint64_t)frame_sz * (uint64_t)o->frame), SEEK_SET) != 0) {
#endif
        const int saved = errno;
        (void)fprintf(stderr, "vmaf-roi: seek to frame %d failed: errno=%d\n", o->frame, saved);
        (void)fclose(fp);
        return -EIO;
    }

    const size_t y_sz = luma_plane_size(o->width, o->height);
    size_t got = 0U;
    const size_t expected = y_sz * sample_bytes_for_bitdepth(o->bitdepth);
    int rc = read_luma8(fp, dst, y_sz, o->bitdepth, &got);
    if (rc < 0) {
        (void)fprintf(stderr, "vmaf-roi: short read at frame %d (%zu of %zu bytes)\n", o->frame,
                      got, expected);
    }
    (void)fclose(fp);
    return rc;
}

/* Center-weighted radial placeholder. Returns saliency in [0, 1]: 1 at the
 * frame centre, falling off to ~0 in the corners. Only used for smoke
 * testing the sidecar plumbing -- NOT a substitute for MobileSal. */
static void fill_placeholder_saliency(int w, int h, float *dst)
{
    const double cx = (double)(w - 1) * 0.5;
    const double cy = (double)(h - 1) * 0.5;
    const double rmax = sqrt(cx * cx + cy * cy);
    const double inv_rmax = (rmax > 0.0) ? (1.0 / rmax) : 0.0;
    for (int y = 0; y < h; ++y) {
        const double dy = (double)y - cy;
        for (int x = 0; x < w; ++x) {
            const double dx = (double)x - cx;
            const double r = sqrt(dx * dx + dy * dy) * inv_rmax;
            const double s = 1.0 - r;
            dst[(size_t)y * (size_t)w + (size_t)x] = (float)((s < 0.0) ? 0.0 : s);
        }
    }
}

/* Run the optional ONNX session against the luma frame. The model is
 * expected to take NCHW [1, 1, H, W] uint8-luma and emit a single-channel
 * map at the same resolution; vmaf_dnn_session_run_luma8() applies the
 * sidecar normalization and writes back uint8 [0, 255], which we then
 * scale to [0, 1]. */
static int run_saliency_model(const struct vmaf_roi_opts *o, const uint8_t *luma, float *dst)
{
    if (vmaf_dnn_available() == 0) {
        (void)fprintf(
            stderr,
            "vmaf-roi: --saliency-model requested but libvmaf was built without DNN support\n");
        return -ENOSYS;
    }

    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, o->saliency_model, NULL);
    if (rc < 0) {
        (void)fprintf(stderr, "vmaf-roi: cannot open saliency model %s: %d\n", o->saliency_model,
                      rc);
        return rc;
    }

    const size_t y_sz = luma_plane_size(o->width, o->height);
    uint8_t *out8 = (uint8_t *)malloc(y_sz);
    if (out8 == NULL) {
        vmaf_dnn_session_close(sess);
        return -ENOMEM;
    }

    rc = vmaf_dnn_session_run_luma8(sess, luma, (size_t)o->width, o->width, o->height, out8,
                                    (size_t)o->width);
    if (rc == 0) {
        for (size_t i = 0U; i < y_sz; ++i) {
            dst[i] = (float)out8[i] * (1.0F / 255.0F);
        }
    }
    free(out8);
    vmaf_dnn_session_close(sess);
    return rc;
}

/* Per-CTU mean reducer + saliency-to-QP-offset mapping live in
 * vmaf_roi_core.h so the unit test can compile against them without
 * dragging libvmaf's link surface in. */
#define reduce_per_ctu vmaf_roi_reduce_per_ctu
#define saliency_to_qp_offset vmaf_roi_saliency_to_qp

/* x265 qpfile-per-frame format used for ROI: one row per CTU row, space-
 * separated signed offsets, terminated by newline. We prepend a single
 * "<frame>" tag so multi-frame concatenation later is unambiguous. */
static int emit_x265(FILE *fp, const struct vmaf_roi_opts *o, const float *grid, int cols, int rows)
{
    if (fprintf(fp, "# vmaf-roi qpfile (x265, --qpfile-style)\n") < 0)
        return -EIO;
    if (fprintf(fp, "# frame=%d ctu=%d cols=%d rows=%d strength=%.3f\n", o->frame, o->ctu_size,
                cols, rows, o->strength) < 0) {
        return -EIO;
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int q =
                saliency_to_qp_offset(grid[(size_t)r * (size_t)cols + (size_t)c], o->strength);
            const char *sep = (c == 0) ? "" : " ";
            if (fprintf(fp, "%s%d", sep, q) < 0)
                return -EIO;
        }
        if (fputc('\n', fp) == EOF)
            return -EIO;
    }
    return 0;
}

/* SVT-AV1 ROI map format: signed-int8 grid, row-major, no header. The
 * encoder reads the map per-frame; we emit exactly one frame's worth. */
static int emit_svtav1(FILE *fp, const struct vmaf_roi_opts *o, const float *grid, int cols,
                       int rows)
{
    const size_t n = (size_t)cols * (size_t)rows;
    int8_t *buf = (int8_t *)malloc(n);
    if (buf == NULL)
        return -ENOMEM;
    for (size_t i = 0U; i < n; ++i) {
        buf[i] = (int8_t)saliency_to_qp_offset(grid[i], o->strength);
    }
    size_t wrote = fwrite(buf, 1U, n, fp);
    free(buf);
    return (wrote == n) ? 0 : -EIO;
}

static int emit_sidecar(const struct vmaf_roi_opts *o, const float *grid, int cols, int rows)
{
    FILE *fp = NULL;
    bool close_fp = false;
    if (strcmp(o->output, "-") == 0) {
        fp = stdout;
    } else {
        fp = vmaf_fopen_utf8(o->output, (o->encoder == VMAF_ROI_ENCODER_SVTAV1) ? "wb" : "w");
        if (fp == NULL) {
            const int saved = errno;
            (void)fprintf(stderr, "vmaf-roi: cannot open %s for writing: errno=%d\n", o->output,
                          saved);
            return -EIO;
        }
        close_fp = true;
    }

    int rc = 0;
    if (o->encoder == VMAF_ROI_ENCODER_X265) {
        rc = emit_x265(fp, o, grid, cols, rows);
    } else {
        rc = emit_svtav1(fp, o, grid, cols, rows);
    }

    if (close_fp) {
        if (fclose(fp) != 0)
            rc = (rc == 0) ? -EIO : rc;
    } else {
        if (fflush(fp) != 0)
            rc = (rc == 0) ? -EIO : rc;
    }
    return rc;
}

static void opts_set_defaults(struct vmaf_roi_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->ctu_size = 64;
    o->bitdepth = 8;
    o->pixfmt = VMAF_ROI_PIXFMT_420;
    o->encoder = VMAF_ROI_ENCODER_X265;
    o->strength = 6.0;
    o->frame = 0;
}

enum {
    OPT_REFERENCE = 1000,
    OPT_OUTPUT,
    OPT_WIDTH,
    OPT_HEIGHT,
    OPT_FRAME,
    OPT_PIXEL_FORMAT,
    OPT_BITDEPTH,
    OPT_CTU_SIZE,
    OPT_ENCODER,
    OPT_STRENGTH,
    OPT_SALIENCY_MODEL,
};

static const struct option g_long_opts[] = {
    {"reference", required_argument, NULL, OPT_REFERENCE},
    {"output", required_argument, NULL, OPT_OUTPUT},
    {"width", required_argument, NULL, OPT_WIDTH},
    {"height", required_argument, NULL, OPT_HEIGHT},
    {"frame", required_argument, NULL, OPT_FRAME},
    {"pixel_format", required_argument, NULL, OPT_PIXEL_FORMAT},
    {"bitdepth", required_argument, NULL, OPT_BITDEPTH},
    {"ctu-size", required_argument, NULL, OPT_CTU_SIZE},
    {"encoder", required_argument, NULL, OPT_ENCODER},
    {"strength", required_argument, NULL, OPT_STRENGTH},
    {"saliency-model", required_argument, NULL, OPT_SALIENCY_MODEL},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

/* Each option group is dispatched through a small helper so parse_args()
 * stays under the Power-of-10 60-line cap. */
static int parse_args_str_opt(int code, struct vmaf_roi_opts *o)
{
    switch (code) {
    case OPT_REFERENCE:
        o->reference = optarg;
        return 0;
    case OPT_OUTPUT:
        o->output = optarg;
        return 0;
    case OPT_SALIENCY_MODEL:
        o->saliency_model = optarg;
        return 0;
    case OPT_PIXEL_FORMAT:
        return parse_pixfmt(optarg, &o->pixfmt);
    case OPT_ENCODER:
        return parse_encoder(optarg, &o->encoder);
    default:
        return -EINVAL;
    }
}

static int parse_args_num_opt(int code, struct vmaf_roi_opts *o)
{
    long lv = 0;
    double dv = 0.0;
    int rc = 0;
    switch (code) {
    case OPT_WIDTH:
        rc = parse_int_arg(optarg, 1, VMAF_ROI_MAX_DIM, &lv);
        o->width = (int)lv;
        return rc;
    case OPT_HEIGHT:
        rc = parse_int_arg(optarg, 1, VMAF_ROI_MAX_DIM, &lv);
        o->height = (int)lv;
        return rc;
    case OPT_FRAME:
        rc = parse_int_arg(optarg, 0, VMAF_ROI_MAX_FRAME_INDEX, &lv);
        o->frame = (int)lv;
        return rc;
    case OPT_BITDEPTH:
        rc = parse_int_arg(optarg, 8, 16, &lv);
        if (rc < 0) {
            (void)fprintf(stderr, "vmaf-roi: --bitdepth must be 8, 10, 12, or 16\n");
            return rc;
        }
        if (lv != 8 && lv != 10 && lv != 12 && lv != 16) {
            (void)fprintf(stderr, "vmaf-roi: --bitdepth must be 8, 10, 12, or 16\n");
            return -ERANGE;
        }
        o->bitdepth = (int)lv;
        return 0;
    case OPT_CTU_SIZE:
        rc = parse_int_arg(optarg, VMAF_ROI_MIN_CTU, VMAF_ROI_MAX_CTU, &lv);
        o->ctu_size = (int)lv;
        return rc;
    case OPT_STRENGTH:
        rc = parse_double_arg(optarg, 0.0, 64.0, &dv);
        o->strength = dv;
        return rc;
    default:
        return -EINVAL;
    }
}

static int parse_args(int argc, char **argv, struct vmaf_roi_opts *o)
{
    opts_set_defaults(o);
    int c = 0;
    /* getopt_long is the standard CLI argument loop; the "not thread safe"
     * warning is inherent to CLI option parsing and matches upstream
     * libvmaf/tools/cli_parse.c. CLI parsing happens before any threads
     * are spawned, so this is safe. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) */
    while ((c = getopt_long(argc, argv, "h", g_long_opts, NULL)) != -1) {
        if (c == 'h') {
            print_usage(stdout);
            return -ECANCELED; /* signal main() to exit cleanly */
        }
        int rc = 0;
        if (c == OPT_REFERENCE || c == OPT_OUTPUT || c == OPT_SALIENCY_MODEL ||
            c == OPT_PIXEL_FORMAT || c == OPT_ENCODER) {
            rc = parse_args_str_opt(c, o);
        } else {
            rc = parse_args_num_opt(c, o);
        }
        if (rc < 0)
            return rc;
    }
    if (o->reference == NULL || o->output == NULL || o->width <= 0 || o->height <= 0) {
        (void)fprintf(stderr, "vmaf-roi: --reference, --output, --width, --height are required\n");
        return -EINVAL;
    }
    return 0;
}

/* Compute saliency for the requested frame: load the luma plane, then
 * either invoke the ONNX model or fall back to the deterministic
 * placeholder. Returns 0 on success and a negative errno on failure. */
static int compute_saliency(const struct vmaf_roi_opts *o, float *sal)
{
    const size_t y_sz = luma_plane_size(o->width, o->height);
    uint8_t *luma = (uint8_t *)malloc(y_sz);
    if (luma == NULL)
        return -ENOMEM;

    int rc = load_luma_frame(o, luma);
    if (rc == 0) {
        if (o->saliency_model != NULL) {
            rc = run_saliency_model(o, luma, sal);
        } else {
            fill_placeholder_saliency(o->width, o->height, sal);
        }
    }
    free(luma);
    return rc;
}

/* Run the full pipeline once parse_args() has populated @p opts. Split
 * out so main() stays under the Power-of-10 60-line cap. */
static int run_pipeline(const struct vmaf_roi_opts *opts)
{
    const size_t y_sz = luma_plane_size(opts->width, opts->height);
    /* calloc zero-initialises so a degenerate/empty plane still produces a
     * defined input to reduce_per_ctu(). compute_saliency() overwrites
     * every cell on the success path. */
    float *sal = (float *)calloc(y_sz, sizeof(float));
    if (sal == NULL) {
        (void)fprintf(stderr, "vmaf-roi: out of memory\n");
        return -ENOMEM;
    }

    int rc = compute_saliency(opts, sal);
    if (rc < 0) {
        free(sal);
        return rc;
    }

    /* Ceiling division -- partial CTUs at the right / bottom are honored. */
    const int cols = (opts->width + opts->ctu_size - 1) / opts->ctu_size;
    const int rows = (opts->height + opts->ctu_size - 1) / opts->ctu_size;
    if (cols <= 0 || rows <= 0) {
        (void)fprintf(stderr, "vmaf-roi: degenerate CTU grid (%d x %d)\n", cols, rows);
        free(sal);
        return -EINVAL;
    }

    float *grid = (float *)malloc((size_t)cols * (size_t)rows * sizeof(float));
    if (grid == NULL) {
        free(sal);
        (void)fprintf(stderr, "vmaf-roi: out of memory\n");
        return -ENOMEM;
    }
    reduce_per_ctu(sal, opts->width, opts->height, opts->ctu_size, grid, cols, rows);
    free(sal);

    rc = emit_sidecar(opts, grid, cols, rows);
    free(grid);
    if (rc < 0)
        (void)fprintf(stderr, "vmaf-roi: failed to emit sidecar: %d\n", rc);
    return rc;
}

int main(int argc, char **argv)
{
    struct vmaf_roi_opts opts;
    int rc = parse_args(argc, argv, &opts);
    if (rc == -ECANCELED)
        return EXIT_SUCCESS; /* --help requested */
    if (rc < 0) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }
    return (run_pipeline(&opts) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
