/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_ms_ssim_cuda reads its input pictures on the device
 * (T-CUDA-MS-SSIM-HOST-STAGING-2026-10-06).
 *
 * The twin copied every scored plane of both pictures to pinned host memory,
 * waited for the copy, converted it with picture_copy() on the host and
 * uploaded the floats again: per plane and frame two plane-sized copies to the
 * host, two uploads and two host waits. Level 0 of its pyramids is now
 * picture_copy() on the device. This test feeds device pictures (what the
 * FFmpeg `libvmaf_cuda` filter hands libvmaf) and counts every copy with a
 * host side that is made while the frames are scored, by wrapping the copy
 * entries of the CUDA state's driver table: nothing is uploaded, no 2D copy
 * has a host side, and what goes to the host is exactly the per-window term
 * planes the host adds in raster order (ADR-1465). Every output stays the
 * CPU's value bit for bit. Two cases: 8-bit 4:4:4 with enable_chroma (three
 * planes, one byte per sample) and 10-bit 4:2:0 (luma, two bytes per sample).
 *
 * The test skips (exit 77) when no CUDA device is visible.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"

#include "cuda/common.h"
#include "cuda/picture_cuda.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FRAME_W 256u
#define FRAME_H 192u
#define NUM_FRAMES 3u
#define MAX_KEYS 3u
#define MS_SSIM_SCALES 5u
#define MS_SSIM_WINDOW 11u

typedef struct Case {
    const char *name;
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    bool chroma;
    unsigned n_keys;
} Case;

static const char *const KEYS[MAX_KEYS] = {"float_ms_ssim", "float_ms_ssim_cb", "float_ms_ssim_cr"};

/* Copies with a host side, counted while `counting` is set. */
typedef struct HostTraffic {
    uint64_t to_device_bytes;
    uint64_t to_host_bytes;
    unsigned plane_copies; /* 2D copies with a host side */
    unsigned unclassified; /* cuMemcpy / cuMemcpyAsync: the direction is not stated */
} HostTraffic;

static HostTraffic traffic;
static bool counting;
static CudaFunctions real; /* the driver entries the wrappers forward to */

static void count_2d(const CUDA_MEMCPY2D *p)
{
    if (!counting)
        return;
    const uint64_t bytes = (uint64_t)p->WidthInBytes * p->Height;
    const bool from_host = p->srcMemoryType == CU_MEMORYTYPE_HOST;
    const bool to_host = p->dstMemoryType == CU_MEMORYTYPE_HOST;
    traffic.plane_copies += (from_host || to_host) ? 1u : 0u;
    traffic.to_device_bytes += from_host ? bytes : 0u;
    traffic.to_host_bytes += to_host ? bytes : 0u;
}

static CUresult CUDAAPI wrap_2d(const CUDA_MEMCPY2D *p)
{
    count_2d(p);
    return real.cuMemcpy2D(p);
}

static CUresult CUDAAPI wrap_2d_async(const CUDA_MEMCPY2D *p, CUstream s)
{
    count_2d(p);
    return real.cuMemcpy2DAsync(p, s);
}

static CUresult CUDAAPI wrap_htod(CUdeviceptr d, const void *h, size_t n)
{
    traffic.to_device_bytes += counting ? n : 0u;
    return real.cuMemcpyHtoD(d, h, n);
}

static CUresult CUDAAPI wrap_htod_async(CUdeviceptr d, const void *h, size_t n, CUstream s)
{
    traffic.to_device_bytes += counting ? n : 0u;
    return real.cuMemcpyHtoDAsync(d, h, n, s);
}

static CUresult CUDAAPI wrap_dtoh(void *h, CUdeviceptr d, size_t n)
{
    traffic.to_host_bytes += counting ? n : 0u;
    return real.cuMemcpyDtoH(h, d, n);
}

static CUresult CUDAAPI wrap_dtoh_async(void *h, CUdeviceptr d, size_t n, CUstream s)
{
    traffic.to_host_bytes += counting ? n : 0u;
    return real.cuMemcpyDtoHAsync(h, d, n, s);
}

static CUresult CUDAAPI wrap_any(CUdeviceptr dst, CUdeviceptr src, size_t n)
{
    traffic.unclassified += counting ? 1u : 0u;
    return real.cuMemcpy(dst, src, n);
}

static CUresult CUDAAPI wrap_any_async(CUdeviceptr dst, CUdeviceptr src, size_t n, CUstream s)
{
    traffic.unclassified += counting ? 1u : 0u;
    return real.cuMemcpyAsync(dst, src, n, s);
}

/* Route the state's copies through the counters. */
static void wrap_copies(CudaFunctions *f)
{
    real = *f;
    f->cuMemcpy2D = wrap_2d;
    f->cuMemcpy2DAsync = wrap_2d_async;
    f->cuMemcpyHtoD = wrap_htod;
    f->cuMemcpyHtoDAsync = wrap_htod_async;
    f->cuMemcpyDtoH = wrap_dtoh;
    f->cuMemcpyDtoHAsync = wrap_dtoh_async;
    f->cuMemcpy = wrap_any;
    f->cuMemcpyAsync = wrap_any_async;
    memset(&traffic, 0, sizeof(traffic));
    counting = false;
}

/* Bytes of float_ms_ssim_cuda's term readback for one frame: per scored plane
 * and scale, an l and a c double and an s float per window (ADR-1465). */
static uint64_t term_bytes_per_frame(const Case *c)
{
    const unsigned n_planes = c->chroma ? 3u : 1u;
    const bool sub = c->pix_fmt == VMAF_PIX_FMT_YUV420P;
    uint64_t bytes = 0u;
    for (unsigned p = 0u; p < n_planes; p++) {
        unsigned w = (p > 0u && sub) ? (FRAME_W + 1u) / 2u : FRAME_W;
        unsigned h = (p > 0u && sub) ? (FRAME_H + 1u) / 2u : FRAME_H;
        for (unsigned i = 0u; i < MS_SSIM_SCALES; i++) {
            const uint64_t windows =
                (uint64_t)(w - (MS_SSIM_WINDOW - 1u)) * (uint64_t)(h - (MS_SSIM_WINDOW - 1u));
            bytes += windows * (2u * sizeof(double) + sizeof(float));
            w = (w / 2u) + (w & 1u);
            h = (h / 2u) + (h & 1u);
        }
    }
    return bytes;
}

/* lowbias32 hash: stateless, so both runs see the same pictures. */
static uint32_t sample_hash(unsigned row, unsigned col, unsigned plane, unsigned frame)
{
    uint32_t x =
        ((uint32_t)row << 16) ^ (uint32_t)col ^ (frame * 0x9E3779B9u) ^ (plane * 0x85EBCA6Bu);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* A ramp with noise in the top eight bits (`distorted` adds a smaller noise)
 * and noise in the bits below them: at most 221 << (bpc - 8), so no sample
 * clips. */
static unsigned sample_value(const Case *c, unsigned row, unsigned col, unsigned plane,
                             unsigned frame, bool distorted)
{
    const unsigned shift = c->bpc - 8u;
    unsigned v =
        ((row * 3u + col + frame * 7u) % 200u) + (sample_hash(row, col, plane, frame) >> 28);
    if (distorted)
        v += sample_hash(row, col, plane + 3u, frame) >> 29;
    const unsigned low = shift ? (sample_hash(row, col, plane + 6u, frame) >> (32u - shift)) : 0u;
    return (v << shift) | low;
}

/* Row `row` of plane `p`: one byte per sample at 8 bits, two above. */
static void fill_row(const Case *c, VmafPicture *pic, unsigned p, unsigned row, unsigned frame,
                     bool distorted)
{
    uint8_t *const line = (uint8_t *)pic->data[p] + ((size_t)row * pic->stride[p]);
    for (unsigned col = 0u; col < pic->w[p]; col++) {
        const unsigned v = sample_value(c, row, col, p, frame, distorted);
        if (c->bpc > 8u) {
            ((uint16_t *)line)[col] = (uint16_t)v;
        } else {
            line[col] = (uint8_t)v;
        }
    }
}

static int fill_picture(const Case *c, VmafPicture *pic, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, c->pix_fmt, c->bpc, FRAME_W, FRAME_H);
    for (unsigned p = 0u; p < 3u && !err; p++) {
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            fill_row(c, pic, p, row, frame, distorted);
        }
    }
    return err;
}

static int read_scores(VmafContext *vmaf, const Case *c, double out[NUM_FRAMES][MAX_KEYS])
{
    assert(c->n_keys <= MAX_KEYS);
    for (unsigned i = 0u; i < NUM_FRAMES; i++) {
        for (unsigned k = 0u; k < c->n_keys && k < MAX_KEYS; k++) {
            const int err = vmaf_feature_score_at_index(vmaf, KEYS[k], &out[i][k], i);
            if (err)
                return err;
        }
    }
    return 0;
}

static int use_ms_ssim(VmafContext *vmaf, const Case *c, const char *name)
{
    VmafFeatureDictionary *opts = NULL;
    if (c->chroma) {
        const int err = vmaf_feature_dictionary_set(&opts, "enable_chroma", "true");
        if (err)
            return err;
    }
    /* vmaf_use_feature() consumes the dictionary, on failure too. */
    return vmaf_use_feature(vmaf, name, opts);
}

/* Flush, read the scores and close. */
static int finish(VmafContext *vmaf, const Case *c, int err, double out[NUM_FRAMES][MAX_KEYS])
{
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    counting = false;
    if (!err)
        err = read_scores(vmaf, c, out);
    if (vmaf) {
        const int close_err = vmaf_close(vmaf);
        err = err ? err : close_err;
    }
    return err;
}

/* The CPU reference: host pictures, the CPU extractor. */
static int score_on_cpu(const Case *c, double out[NUM_FRAMES][MAX_KEYS])
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = use_ms_ssim(vmaf, c, "float_ms_ssim");
    for (unsigned i = 0u; i < NUM_FRAMES && !err; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_picture(c, &ref, i, false);
        if (!err)
            err = fill_picture(c, &dist, i, true);
        if (!err)
            err = vmaf_read_pictures(vmaf, &ref, &dist, i);
    }
    return finish(vmaf, c, err, out);
}

/* A device picture of the pool, filled from a host picture (not counted). */
static int fetch_device_picture(VmafContext *vmaf, const VmafCudaState *state, const Case *c,
                                unsigned frame, bool distorted, VmafPicture *dev)
{
    VmafPicture host;
    int err = fill_picture(c, &host, frame, distorted);
    if (err)
        return err;
    err = vmaf_cuda_fetch_preallocated_picture(vmaf, dev);
    if (!err)
        err = vmaf_cuda_picture_upload_async(dev, &host, 0x7);
    if (!err && state->f->cuStreamSynchronize(vmaf_cuda_picture_get_stream(dev)) != CUDA_SUCCESS)
        err = -EIO;
    const int unref_err = vmaf_picture_unref(&host);
    return err ? err : unref_err;
}

static int open_device_run(VmafCudaState *state, const Case *c, VmafContext **out)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(out, cfg);
    if (!err)
        err = vmaf_cuda_import_state(*out, state);
    const VmafCudaPictureConfiguration pool = {
        .pic_params = {.w = FRAME_W, .h = FRAME_H, .bpc = c->bpc, .pix_fmt = c->pix_fmt},
        .pic_prealloc_method = VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE,
    };
    if (!err)
        err = vmaf_cuda_preallocate_pictures(*out, pool);
    if (!err)
        err = use_ms_ssim(*out, c, "float_ms_ssim_cuda");
    return err;
}

/* The CUDA run: device pictures; copies are counted while frames are scored. */
static int score_on_device(VmafCudaState *state, const Case *c, double out[NUM_FRAMES][MAX_KEYS])
{
    VmafContext *vmaf = NULL;
    int err = open_device_run(state, c, &vmaf);
    for (unsigned i = 0u; i < NUM_FRAMES && !err; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fetch_device_picture(vmaf, state, c, i, false, &ref);
        if (!err)
            err = fetch_device_picture(vmaf, state, c, i, true, &dist);
        counting = true;
        if (!err)
            err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        counting = false;
    }
    counting = true; /* the flush scores the last frames */
    return finish(vmaf, c, err, out);
}

/* The bits of a double, so +0 and -0 or two NaNs are told apart. */
static uint64_t double_bits(double v)
{
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static char *compare_with_cpu(const Case *c, double cpu[NUM_FRAMES][MAX_KEYS],
                              double dev[NUM_FRAMES][MAX_KEYS])
{
    assert(c->n_keys <= MAX_KEYS);
    for (unsigned i = 0u; i < NUM_FRAMES; i++) {
        for (unsigned k = 0u; k < c->n_keys && k < MAX_KEYS; k++) {
            if (double_bits(cpu[i][k]) != double_bits(dev[i][k])) {
                (void)fprintf(stderr, "\n  %s frame %u %s: cpu=%.17g cuda=%.17g\n", c->name, i,
                              KEYS[k], cpu[i][k], dev[i][k]);
                return "float_ms_ssim_cuda is not the CPU's value bit for bit";
            }
        }
    }
    return NULL;
}

static char *check_traffic(const Case *c)
{
    const uint64_t terms = term_bytes_per_frame(c) * NUM_FRAMES;
    (void)fprintf(stderr,
                  "[%s: %llu bytes to the device, %llu to the host (terms %llu), "
                  "%u plane copies with a host side, %u unclassified] ",
                  c->name, (unsigned long long)traffic.to_device_bytes,
                  (unsigned long long)traffic.to_host_bytes, (unsigned long long)terms,
                  traffic.plane_copies, traffic.unclassified);
    mu_assert("float_ms_ssim_cuda uploads while scoring", traffic.to_device_bytes == 0u);
    mu_assert("float_ms_ssim_cuda copies a picture plane to or from the host",
              traffic.plane_copies == 0u);
    mu_assert("a copy of unstated direction while scoring", traffic.unclassified == 0u);
    mu_assert("what goes to the host is not exactly the term planes",
              traffic.to_host_bytes == terms);
    return NULL;
}

static char *run_case(const Case *c)
{
    static double cpu[NUM_FRAMES][MAX_KEYS];
    static double dev[NUM_FRAMES][MAX_KEYS];
    VmafCudaState *state = NULL;
    const VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&state, cuda_cfg) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1;
        return NULL;
    }
    wrap_copies(state->f);
    mu_assert("the CPU reference run failed", score_on_cpu(c, cpu) == 0);
    const int err = score_on_device(state, c, dev);
    (void)vmaf_cuda_state_free(state);
    if (err) {
        (void)fprintf(stderr, "\n  %s: the CUDA run failed with %d\n", c->name, err);
        return "the CUDA run failed";
    }
    char *msg = compare_with_cpu(c, cpu, dev);
    return msg ? msg : check_traffic(c);
}

static char *test_444_8bit_three_planes(void)
{
    static const Case c = {"8-bit 4:4:4 enable_chroma", VMAF_PIX_FMT_YUV444P, 8u, true, 3u};
    return run_case(&c);
}

static char *test_420_10bit_luma(void)
{
    static const Case c = {"10-bit 4:2:0", VMAF_PIX_FMT_YUV420P, 10u, false, 1u};
    return run_case(&c);
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_444_8bit_three_planes),
        MU_TEST(test_420_10bit_luma),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
