/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 */

/*
 * float_motion_metal computes what the CPU float_motion extractor computes,
 * checked without an Apple device (ADR-1498).
 *
 * The kernels of core/src/feature/metal/float_motion.metal compute every
 * value through core/src/feature/metal/metal_float_motion_math.h, and
 * float_motion_metal.mm finishes each plane through the host part of the
 * same header. This test compiles that header as C++ (Metal Shading Language
 * is C++) and runs the kernels' loops on the host, index arithmetic included,
 * against the CPU's own functions, bit for bit:
 *
 *  - the reflect-101 fold against convolution_reflect101(), every index of
 *    every size from 1 to 70;
 *  - the taps against FILTER_5_s, FILTER_3_s and FILTER_5_NO_OP_s;
 *  - the sample conversion against picture_copy() and the blurred plane
 *    against convolution_f32_c_s() and its scalar passes, at 8 to 16 bits,
 *    every motion_filter_size, odd sizes and the CPU's smallest planes;
 *  - the row sums and plane score (motion_add_scale1 included) against
 *    motion.c::compute_motion();
 *  - the whole frame score against the CPU extractor's own
 *    VMAF_feature_motion_score through libvmaf, with motion_add_scale1,
 *    motion_add_uv and motion_filter_size;
 *  - and that the fixtures tell the CPU's order from a per-block sum (the
 *    reduction the twin used before; 1.36e-4 off on 1080p checkerboards).
 *
 * What only an Apple GPU shows is that its fp32 + - * / are the correctly
 * rounded operations the specification promises under -fno-fast-math.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

extern "C" {
#include "feature/common/convolution.h"
#include "feature/common/convolution_internal.h"
#include "feature/motion.h"
#include "feature/motion_tools.h"
#include "feature/picture_copy.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"
#include "mem.h"
}

#include "feature/metal/metal_float_motion_math.h"

namespace
{

/* A float plane in the CPU extractor's layout: rows of ALIGN_CEIL(w * 4)
 * bytes, 32-byte aligned (convolution_f32_avx_s() loads aligned rows). */
struct FmPlane {
    unsigned w;
    unsigned h;
    unsigned stride; /* floats per row */
    float *data;
};

int fm_plane_alloc(FmPlane *p, unsigned w, unsigned h)
{
    p->w = w;
    p->h = h;
    p->stride = (unsigned)(ALIGN_CEIL(w * sizeof(float)) / sizeof(float));
    p->data = static_cast<float *>(aligned_malloc((size_t)p->stride * h * sizeof(float), 32));
    return (p->data != nullptr) ? 0 : -1;
}

void fm_plane_free(FmPlane *p)
{
    aligned_free(p->data);
    p->data = nullptr;
}

uint32_t fm_bits(float v)
{
    uint32_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* lowbias32 of the position, frame and plane: stateless, so every run sees
 * the same pictures. */
uint32_t fm_hash(unsigned x, unsigned y, unsigned frame, unsigned plane)
{
    uint32_t v = ((uint32_t)y << 16) ^ (uint32_t)x ^ (frame * 0x9E3779B9u) ^ (plane * 0x85EBCA6Bu);
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

/* A YUV420P picture of noise at `bpc` bits: frame `frame` of the fixture. */
int fm_picture(VmafPicture *pic, unsigned w, unsigned h, unsigned bpc, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, w, h);
    for (unsigned c = 0; err == 0 && c < 3u; c++) {
        for (unsigned y = 0; y < pic->h[c]; y++) {
            uint8_t *row = static_cast<uint8_t *>(pic->data[c]) + (ptrdiff_t)y * pic->stride[c];
            for (unsigned x = 0; x < pic->w[c]; x++) {
                const uint32_t code = fm_hash(x, y, frame, c) >> (32u - bpc);
                if (bpc <= 8u) {
                    row[x] = (uint8_t)code;
                } else {
                    reinterpret_cast<uint16_t *>(row)[x] = (uint16_t)code;
                }
            }
        }
    }
    return err;
}

/* The kernel's sample load: vmaf_mtl_fm_sample() of every sample of plane
 * `c`, packed `w` per row. */
float *fm_twin_samples(const VmafPicture *pic, unsigned c)
{
    const unsigned w = pic->w[c];
    const unsigned h = pic->h[c];
    const float inv_scaler = vmaf_mtl_fm_inv_scaler(pic->bpc);
    float *out = static_cast<float *>(malloc((size_t)w * h * sizeof(float)));
    for (unsigned y = 0; out != nullptr && y < h; y++) {
        const uint8_t *row =
            static_cast<const uint8_t *>(pic->data[c]) + (ptrdiff_t)y * pic->stride[c];
        for (unsigned x = 0; x < w; x++) {
            const vmaf_mtl_u32 raw =
                (pic->bpc <= 8u) ? row[x] : reinterpret_cast<const uint16_t *>(row)[x];
            out[(size_t)y * w + x] = vmaf_mtl_fm_sample(raw, inv_scaler);
        }
    }
    return out;
}

/* The window of output (lx, ly) of a threadgroup, from its tile. */
VmafMtlFmWindow fm_window(const float *tile, unsigned lx, unsigned ly)
{
    VmafMtlFmWindow win;
    for (unsigned r = 0u; r < (unsigned)VMAF_MTL_FM_TAPS; r++) {
        for (unsigned c = 0u; c < (unsigned)VMAF_MTL_FM_TAPS; c++) {
            win.v[r * (unsigned)VMAF_MTL_FM_TAPS + c] = tile[(ly + r) * VMAF_MTL_FM_TILE + lx + c];
        }
    }
    return win;
}

/* One threadgroup of float_motion_blur: the tile load of all its threads,
 * then the blur of each thread's sample, with the kernel's index arithmetic. */
void fm_twin_blur_group(const float *samples, unsigned w, unsigned h, VmafMtlFmTaps taps,
                        unsigned bx, unsigned by, float *out)
{
    float tile[VMAF_MTL_FM_TILE * VMAF_MTL_FM_TILE];
    const int tile_ox = (int)(bx * VMAF_MTL_FM_BLOCK) - VMAF_MTL_FM_RADIUS;
    const int tile_oy = (int)(by * VMAF_MTL_FM_BLOCK) - VMAF_MTL_FM_RADIUS;
    for (unsigned i = 0u; i < VMAF_MTL_FM_TILE * VMAF_MTL_FM_TILE; i++) {
        const int sy = vmaf_mtl_fm_reflect101(tile_oy + (int)(i / VMAF_MTL_FM_TILE), (int)h);
        const int sx = vmaf_mtl_fm_reflect101(tile_ox + (int)(i % VMAF_MTL_FM_TILE), (int)w);
        tile[i] = samples[(size_t)sy * w + (size_t)sx];
    }
    for (unsigned ly = 0u; ly < VMAF_MTL_FM_BLOCK; ly++) {
        for (unsigned lx = 0u; lx < VMAF_MTL_FM_BLOCK; lx++) {
            const unsigned x = bx * VMAF_MTL_FM_BLOCK + lx;
            const unsigned y = by * VMAF_MTL_FM_BLOCK + ly;
            if (x < w && y < h) {
                out[(size_t)y * w + x] = vmaf_mtl_fm_blur(taps, fm_window(tile, lx, ly));
            }
        }
    }
}

/* float_motion_blur over the whole plane: packed `w` x `h` blurred samples. */
float *fm_twin_blur(const float *samples, unsigned w, unsigned h, unsigned filter_size)
{
    float *out = static_cast<float *>(malloc((size_t)w * h * sizeof(float)));
    const VmafMtlFmTaps taps = vmaf_mtl_fm_taps(filter_size);
    for (unsigned by = 0u; out != nullptr && by * VMAF_MTL_FM_BLOCK < h; by++) {
        for (unsigned bx = 0u; bx * VMAF_MTL_FM_BLOCK < w; bx++) {
            fm_twin_blur_group(samples, w, h, taps, bx, by, out);
        }
    }
    return out;
}

/* float_motion.c::motion_blur_plane(): convolution_f32_c_s() with the filter
 * of `filter_size`, or its two scalar passes when `scalar`. */
int fm_cpu_blur(const FmPlane *src, FmPlane *dst, unsigned filter_size, bool scalar)
{
    const float *filter = FILTER_5_s;
    int taps = 5;
    if (filter_size == 1u) {
        filter = FILTER_5_NO_OP_s;
    } else if (filter_size == 3u) {
        filter = FILTER_3_s;
        taps = 3;
    }
    FmPlane tmp = {};
    if (fm_plane_alloc(&tmp, src->w, src->h) != 0) {
        return -1;
    }
    const int w = (int)src->w;
    const int h = (int)src->h;
    const int stride = (int)src->stride;
    if (scalar) {
        convolution_y_c_s(filter, taps, src->data, tmp.data, w, h, stride, stride, 1);
        convolution_x_c_s(filter, taps, tmp.data, dst->data, w, h, stride, stride, 1);
    } else {
        convolution_f32_c_s(filter, taps, src->data, dst->data, tmp.data, w, h, stride, stride);
    }
    fm_plane_free(&tmp);
    return 0;
}

/* Samples of the packed twin plane that are not the CPU plane's bits. */
unsigned fm_mismatches(const float *twin, const FmPlane *cpu)
{
    unsigned bad = 0u;
    for (unsigned y = 0u; y < cpu->h; y++) {
        for (unsigned x = 0u; x < cpu->w; x++) {
            bad += (fm_bits(twin[(size_t)y * cpu->w + x]) !=
                    fm_bits(cpu->data[(size_t)y * cpu->stride + x])) ?
                       1u :
                       0u;
        }
    }
    return bad;
}

/* The CPU's float plane of luma, its blur (dispatched and scalar) and the
 * twin's, of one fixture; the number of differing samples of each. */
struct FmBlurResult {
    unsigned samples_bad;
    unsigned blur_bad;
    unsigned scalar_bad;
};

int fm_blur_compare(unsigned w, unsigned h, unsigned bpc, unsigned filter_size, FmBlurResult *res)
{
    VmafPicture pic = {};
    FmPlane src = {};
    FmPlane cpu = {};
    FmPlane scalar = {};
    int err = fm_picture(&pic, w, h, bpc, 0u);
    err = err ? err : fm_plane_alloc(&src, w, h);
    err = err ? err : fm_plane_alloc(&cpu, w, h);
    err = err ? err : fm_plane_alloc(&scalar, w, h);
    float *twin = nullptr;
    float *blurred = nullptr;
    if (err == 0) {
        picture_copy(src.data, (ptrdiff_t)(src.stride * sizeof(float)), &pic, -128, bpc, 0);
        twin = fm_twin_samples(&pic, 0u);
        blurred = twin ? fm_twin_blur(twin, w, h, filter_size) : nullptr;
        err = (blurred != nullptr) ? 0 : -1;
    }
    err = err ? err : fm_cpu_blur(&src, &cpu, filter_size, false);
    err = err ? err : fm_cpu_blur(&src, &scalar, filter_size, true);
    if (err == 0) {
        res->samples_bad = fm_mismatches(twin, &src);
        res->blur_bad = fm_mismatches(blurred, &cpu);
        res->scalar_bad = fm_mismatches(blurred, &scalar);
    }
    free(blurred);
    free(twin);
    fm_plane_free(&scalar);
    fm_plane_free(&cpu);
    fm_plane_free(&src);
    if (pic.data[0] != nullptr) {
        (void)vmaf_picture_unref(&pic);
    }
    return err;
}

/* 0 when the twin's samples and blur are the CPU's bits, else 1 (reported). */
int fm_blur_case(unsigned w, unsigned h, unsigned bpc, unsigned filter_size)
{
    FmBlurResult res = {};
    if (fm_blur_compare(w, h, bpc, filter_size, &res) != 0) {
        (void)fprintf(stderr, "\n%ux%u %u-bit mfs=%u: a run failed", w, h, bpc, filter_size);
        return 1;
    }
    if (res.samples_bad == 0u && res.blur_bad == 0u && res.scalar_bad == 0u) {
        return 0;
    }
    (void)fprintf(stderr,
                  "\n%ux%u %u-bit mfs=%u: %u samples, %u blurred (dispatched), %u blurred "
                  "(scalar) differ",
                  w, h, bpc, filter_size, res.samples_bad, res.blur_bad, res.scalar_bad);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Row sums                                                           */
/* ------------------------------------------------------------------ */

/* float_motion_row_sum over a transposed plane: `h` row sums. */
float *fm_twin_row_sums(const float *diff, unsigned w, unsigned h)
{
    float *rows = static_cast<float *>(malloc((size_t)h * sizeof(float)));
    for (unsigned y = 0u; rows != nullptr && y < h; y++) {
        const vmaf_mtl_u32 base = vmaf_mtl_fm_diff_index(0u, y, w);
        float accum = 0.0f;
        for (unsigned j = 0u; j < w; j++) {
            accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];
        }
        rows[y] = accum;
    }
    return rows;
}

/* The scale-0 row sums of two packed planes as the kernels form them: the
 * blur kernel's |cur - prev| into the transposed plane, then the row
 * kernel. */
float *fm_twin_rows(const float *cur, const float *prev, unsigned w, unsigned h)
{
    float *diff = static_cast<float *>(calloc((size_t)vmaf_mtl_fm_diff_count(w, h), sizeof(float)));
    if (diff == nullptr) {
        return nullptr;
    }
    for (unsigned y = 0u; y < h; y++) {
        for (unsigned x = 0u; x < w; x++) {
            const size_t off = (size_t)y * w + x;
            diff[vmaf_mtl_fm_diff_index(x, y, w)] = vmaf_mtl_fm_abs_diff(cur[off], prev[off]);
        }
    }
    float *rows = fm_twin_row_sums(diff, w, h);
    free(diff);
    return rows;
}

/* The four samples of `plane` float_motion.metal's fm_corners() reads. */
VmafMtlFmCorners fm_corners(const float *plane, unsigned w, VmafMtlFmBilinearAt at)
{
    const size_t row1 = (size_t)at.y1 * w;
    const size_t row2 = (size_t)at.y2 * w;
    VmafMtlFmCorners s;
    s.s11 = plane[row1 + (size_t)at.x1];
    s.s12 = plane[row1 + (size_t)at.x2];
    s.s21 = plane[row2 + (size_t)at.x1];
    s.s22 = plane[row2 + (size_t)at.x2];
    return s;
}

/* The scale-1 row sums as the kernels form them: float_motion_scale1_diff
 * into the transposed half-size plane, then the row kernel. */
float *fm_twin_scale1_rows(const float *cur, const float *prev, unsigned w, unsigned h, unsigned sw,
                           unsigned sh)
{
    float *diff =
        static_cast<float *>(calloc((size_t)vmaf_mtl_fm_diff_count(sw, sh), sizeof(float)));
    if (diff == nullptr) {
        return nullptr;
    }
    const float ratio_x = vmaf_mtl_fm_scale_ratio(w, sw);
    const float ratio_y = vmaf_mtl_fm_scale_ratio(h, sh);
    for (unsigned y = 0u; y < sh; y++) {
        for (unsigned x = 0u; x < sw; x++) {
            const VmafMtlFmBilinearAt at = vmaf_mtl_fm_bilinear_at(w, h, ratio_x, ratio_y, x, y);
            const float c = vmaf_mtl_fm_bilinear(fm_corners(cur, w, at), at.dx, at.dy);
            const float p = vmaf_mtl_fm_bilinear(fm_corners(prev, w, at), at.dx, at.dy);
            diff[vmaf_mtl_fm_diff_index(x, y, sw)] = vmaf_mtl_fm_abs_diff(c, p);
        }
    }
    float *rows = fm_twin_row_sums(diff, sw, sh);
    free(diff);
    return rows;
}

/* The plane score as the twin forms it: the kernels' row sums, then the
 * host tail of float_motion_metal.mm. */
int fm_twin_plane_score(const float *cur, const float *prev, unsigned w, unsigned h, bool scale1,
                        double *score)
{
    const unsigned sw = vmaf_mtl_fm_scaled_extent(w);
    const unsigned sh = vmaf_mtl_fm_scaled_extent(h);
    float *rows = fm_twin_rows(cur, prev, w, h);
    float *rows1 = scale1 ? fm_twin_scale1_rows(cur, prev, w, h, sw, sh) : nullptr;
    const bool ok = rows != nullptr && (rows1 != nullptr || !scale1);
    if (ok) {
        *score = vmaf_mtl_fm_plane_score(rows, w, h, rows1, sw, sh);
    }
    free(rows);
    free(rows1);
    return ok ? 0 : -1;
}

/* Two blurred-looking planes: a ramp in [-128, 127] with a fractional part,
 * and a second frame up to 4 units off, so the absolute differences carry
 * full fp32 mantissas and the running sums round at every step. */
int fm_pair(unsigned w, unsigned h, unsigned seed, float **cur, float **prev)
{
    *cur = static_cast<float *>(malloc((size_t)w * h * sizeof(float)));
    *prev = static_cast<float *>(malloc((size_t)w * h * sizeof(float)));
    if (*cur == nullptr || *prev == nullptr) {
        return -1;
    }
    for (unsigned y = 0u; y < h; y++) {
        for (unsigned x = 0u; x < w; x++) {
            const size_t i = (size_t)y * w + x;
            const float base = (float)(((x * 3u) + (y * 5u)) & 255u) - 128.0f;
            const float fine = (float)(fm_hash(x, y, seed, 0u) & 0xFFFFu) * (1.0f / 65536.0f);
            const float move = ((float)(fm_hash(x, y, seed, 1u) & 0xFFFu) * (1.0f / 512.0f)) - 4.0f;
            (*cur)[i] = base + fine;
            (*prev)[i] = base + fine + move;
        }
    }
    return 0;
}

/* 0 when the twin's plane score of a `w` x `h` pair is compute_motion()'s
 * bit for bit, 1 when it differs (reported), -1 when a run failed. */
int fm_rows_case(unsigned w, unsigned h, bool scale1, unsigned seed)
{
    float *cur = nullptr;
    float *prev = nullptr;
    double cpu = 0.0;
    double twin = 0.0;
    const int stride = (int)(w * sizeof(float));
    int err = fm_pair(w, h, seed, &cur, &prev);
    /* The CPU's order of the operands: the previous blurred frame first. */
    err =
        err ? err : compute_motion(prev, cur, (int)w, (int)h, stride, stride, &cpu, scale1 ? 1 : 0);
    err = err ? err : fm_twin_plane_score(cur, prev, w, h, scale1, &twin);
    free(cur);
    free(prev);
    if (err != 0 || !(cpu > 0.0)) {
        return -1;
    }
    if (cpu == twin) {
        return 0;
    }
    (void)fprintf(stderr, "\n%ux%u scale1=%d: cpu=%.17g twin=%.17g", w, h, (int)scale1, cpu, twin);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The whole frame score against the CPU extractor                    */
/* ------------------------------------------------------------------ */

struct FmCase {
    unsigned w;
    unsigned h;
    unsigned bpc;
    const char *opts[5]; /* key, value, ...; nullptr-terminated */
    const char *key;     /* the debug motion score's name under `opts` */
    unsigned filter_size;
    bool scale1;
    bool uv;
};

/* The twin's score of plane `c` between frames 0 and 1. */
int fm_twin_case_plane(const VmafPicture pics[2], unsigned c, const FmCase *k, double *score)
{
    const unsigned w = pics[0].w[c];
    const unsigned h = pics[0].h[c];
    float *s0 = fm_twin_samples(&pics[0], c);
    float *s1 = fm_twin_samples(&pics[1], c);
    float *b0 = s0 ? fm_twin_blur(s0, w, h, k->filter_size) : nullptr;
    float *b1 = s1 ? fm_twin_blur(s1, w, h, k->filter_size) : nullptr;
    const int err = (b0 && b1) ? fm_twin_plane_score(b1, b0, w, h, k->scale1, score) : -1;
    free(b1);
    free(b0);
    free(s1);
    free(s0);
    return err;
}

/* The twin's frame-1 score: the planes added in double, as
 * float_motion_metal.mm::fm_metal_frame_score() adds them. */
int fm_twin_case_score(const FmCase *k, double *score)
{
    VmafPicture pics[2] = {};
    int err = fm_picture(&pics[0], k->w, k->h, k->bpc, 0u);
    err = err ? err : fm_picture(&pics[1], k->w, k->h, k->bpc, 1u);
    const unsigned n_planes = k->uv ? 3u : 1u;
    *score = 0.0;
    for (unsigned c = 0u; err == 0 && c < n_planes; c++) {
        double plane = 0.0;
        err = fm_twin_case_plane(pics, c, k, &plane);
        *score += plane;
    }
    for (unsigned f = 0u; f < 2u; f++) {
        if (pics[f].data[0] != nullptr) {
            (void)vmaf_picture_unref(&pics[f]);
        }
    }
    return err;
}

int fm_use_float_motion(VmafContext *vmaf, const char *const *opts)
{
    VmafFeatureDictionary *dict = nullptr;
    for (unsigned i = 0u; opts[i] != nullptr; i += 2u) {
        const int err = vmaf_feature_dictionary_set(&dict, opts[i], opts[i + 1u]);
        if (err != 0) {
            (void)vmaf_feature_dictionary_free(&dict);
            return err;
        }
    }
    /* vmaf_use_feature() takes the dictionary over, on failure too. */
    return vmaf_use_feature(vmaf, "float_motion", dict);
}

/* Frames 0 and 1 through the CPU float_motion extractor: its frame-1 debug
 * score, the frame SAD itself with the default fps weight and cap. */
int fm_cpu_case_score(const FmCase *k, double *score)
{
    VmafContext *vmaf = nullptr;
    VmafConfiguration cfg = {};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    int err = vmaf_init(&vmaf, cfg);
    err = err ? err : fm_use_float_motion(vmaf, k->opts);
    for (unsigned f = 0u; err == 0 && f < 2u; f++) {
        VmafPicture ref = {};
        VmafPicture dist = {};
        err = fm_picture(&ref, k->w, k->h, k->bpc, f);
        err = err ? err : fm_picture(&dist, k->w, k->h, k->bpc, f);
        err = err ? err : vmaf_read_pictures(vmaf, &ref, &dist, f);
    }
    err = err ? err : vmaf_read_pictures(vmaf, nullptr, nullptr, 0);
    err = err ? err : vmaf_feature_score_at_index(vmaf, k->key, score, 1u);
    const int closed = (vmaf != nullptr) ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* 0 when the twin's frame score is the CPU extractor's bit for bit, 1 when
 * it differs (reported), -1 when a run failed. */
int fm_frame_case(const FmCase *k)
{
    double cpu = 0.0;
    double twin = 0.0;
    if (fm_cpu_case_score(k, &cpu) != 0 || fm_twin_case_score(k, &twin) != 0 || !(cpu > 0.0)) {
        (void)fprintf(stderr, "\n%s %ux%u %u-bit: a run failed", k->key, k->w, k->h, k->bpc);
        return -1;
    }
    if (cpu == twin) {
        return 0;
    }
    (void)fprintf(stderr, "\n%s %ux%u %u-bit: cpu=%.17g twin=%.17g", k->key, k->w, k->h, k->bpc,
                  cpu, twin);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The comparison has teeth                                           */
/* ------------------------------------------------------------------ */

/* The reduction the twin used before ADR-1409: one fp32 sum per 16x16
 * block (simd_sum, then a serial sum of the SIMD groups), the blocks added
 * in double. */
double fm_block_score(const float *cur, const float *prev, unsigned w, unsigned h)
{
    double total = 0.0;
    for (unsigned by = 0u; by < h; by += 16u) {
        for (unsigned bx = 0u; bx < w; bx += 16u) {
            float block = 0.0f;
            for (unsigned y = by; y < by + 16u && y < h; y++) {
                for (unsigned x = bx; x < bx + 16u && x < w; x++) {
                    block += vmaf_mtl_fm_abs_diff(cur[(size_t)y * w + x], prev[(size_t)y * w + x]);
                }
            }
            total += (double)block;
        }
    }
    return total / ((double)w * (double)h);
}

/* ------------------------------------------------------------------ */
/* Cases                                                              */
/* ------------------------------------------------------------------ */

mu_message_t test_reflect101_is_the_cpu_fold()
{
    unsigned bad = 0u;
    for (int size = 1; size <= 70; size++) {
        for (int idx = -150; idx <= 220; idx++) {
            bad +=
                (vmaf_mtl_fm_reflect101(idx, size) != convolution_reflect101(idx, size)) ? 1u : 0u;
        }
    }
    mu_assert("vmaf_mtl_fm_reflect101() is not convolution_reflect101()", bad == 0u);
    return nullptr;
}

/* Tap k of the twin's filter for `filter_size` against the CPU's array. */
unsigned fm_tap_mismatches(unsigned filter_size, const float *cpu, unsigned cpu_taps)
{
    const VmafMtlFmTaps t = vmaf_mtl_fm_taps(filter_size);
    const unsigned pad = ((unsigned)VMAF_MTL_FM_TAPS - cpu_taps) / 2u;
    unsigned bad = 0u;
    for (unsigned k = 0u; k < (unsigned)VMAF_MTL_FM_TAPS; k++) {
        const bool inner = k >= pad && k < pad + cpu_taps;
        const float want = inner ? cpu[k - pad] : 0.0f;
        bad += (fm_bits(t.w[k]) != fm_bits(want)) ? 1u : 0u;
    }
    return bad;
}

mu_message_t test_taps_are_the_cpu_filters()
{
    mu_assert("filter 5 is not FILTER_5_s", fm_tap_mismatches(5u, FILTER_5_s, 5u) == 0u);
    mu_assert("filter 0 is not FILTER_5_s", fm_tap_mismatches(0u, FILTER_5_s, 5u) == 0u);
    mu_assert("filter 9 is not FILTER_5_s", fm_tap_mismatches(9u, FILTER_5_s, 5u) == 0u);
    mu_assert("filter 3 is not FILTER_3_s", fm_tap_mismatches(3u, FILTER_3_s, 3u) == 0u);
    mu_assert("filter 1 is not FILTER_5_NO_OP_s",
              fm_tap_mismatches(1u, FILTER_5_NO_OP_s, 5u) == 0u);
    return nullptr;
}

/* Odd sizes, one threadgroup and a few, 17 (one sample past a group), and
 * the CPU's smallest planes: 3x3 for the 5-tap filters, 2x2 for the 3-tap. */
mu_message_t test_blur_is_the_cpu_convolution()
{
    static const unsigned sizes[][2] = {{3u, 3u},   {4u, 3u},  {3u, 7u},   {5u, 5u},
                                        {17u, 17u}, {33u, 9u}, {16u, 16u}, {161u, 91u}};
    static const unsigned filters[] = {5u, 0u, 3u, 1u};
    static const unsigned depths[] = {8u, 10u, 12u, 16u};
    unsigned bad = 0u;
    for (const auto &size : sizes) {
        for (const unsigned filter : filters) {
            for (const unsigned bpc : depths) {
                bad += (unsigned)fm_blur_case(size[0], size[1], bpc, filter);
            }
        }
    }
    bad += (unsigned)fm_blur_case(2u, 2u, 8u, 3u);
    bad += (unsigned)fm_blur_case(2u, 5u, 10u, 3u);
    bad += (unsigned)fm_blur_case(7u, 2u, 16u, 3u);
    mu_assert("the twin's samples or blur are not the CPU's bits", bad == 0u);
    return nullptr;
}

mu_message_t test_row_sums_are_compute_motion()
{
    mu_assert("640x360", fm_rows_case(640u, 360u, false, 1u) == 0);
    mu_assert("1920x1080", fm_rows_case(1920u, 1080u, false, 2u) == 0);
    mu_assert("333x127 (odd)", fm_rows_case(333u, 127u, false, 3u) == 0);
    mu_assert("3x3", fm_rows_case(3u, 3u, false, 4u) == 0);
    mu_assert("2x2", fm_rows_case(2u, 2u, false, 5u) == 0);
    return nullptr;
}

/* motion_add_scale1: the half-size bilinear planes, their row sums, and the
 * fp32 sum of the two means. Odd sizes make the scaler mirror at the right
 * and bottom edges; 2x2 scales to one sample. */
mu_message_t test_scale1_row_sums_are_compute_motion()
{
    mu_assert("640x360 scale1", fm_rows_case(640u, 360u, true, 6u) == 0);
    mu_assert("1920x1080 scale1", fm_rows_case(1920u, 1080u, true, 7u) == 0);
    mu_assert("333x127 scale1 (odd)", fm_rows_case(333u, 127u, true, 8u) == 0);
    mu_assert("3x3 scale1", fm_rows_case(3u, 3u, true, 9u) == 0);
    mu_assert("2x2 scale1", fm_rows_case(2u, 2u, true, 10u) == 0);
    return nullptr;
}

mu_message_t test_frame_score_is_the_cpu_extractor()
{
    static const FmCase cases[] = {
        {161u, 91u, 8u, {nullptr}, "VMAF_feature_motion_score", 5u, false, false},
        {161u, 91u, 10u, {nullptr}, "VMAF_feature_motion_score", 5u, false, false},
        {64u, 36u, 16u, {nullptr}, "VMAF_feature_motion_score", 5u, false, false},
        {161u,
         91u,
         8u,
         {"motion_add_scale1", "true", "motion_add_uv", "true", nullptr},
         "motion_mdc_mau",
         5u,
         true,
         true},
        {37u, 23u, 12u, {"motion_add_uv", "true", nullptr}, "motion_mau", 5u, false, true},
        {161u, 91u, 8u, {"motion_filter_size", "3", nullptr}, "motion_mfs_3", 3u, false, false},
        {161u, 91u, 10u, {"motion_filter_size", "1", nullptr}, "motion_mfs_1", 1u, false, false},
    };
    unsigned bad = 0u;
    for (const FmCase &k : cases) {
        bad += (fm_frame_case(&k) != 0) ? 1u : 0u;
    }
    mu_assert("the twin's frame score is not the CPU extractor's", bad == 0u);
    return nullptr;
}

/* On this fixture the per-block reduction is not the CPU's score, so the
 * comparisons above would see it. */
mu_message_t test_block_reduction_is_detected()
{
    float *cur = nullptr;
    float *prev = nullptr;
    double cpu = 0.0;
    const unsigned w = 1920u;
    const unsigned h = 1080u;
    const int stride = (int)(w * sizeof(float));
    int err = fm_pair(w, h, 2u, &cur, &prev);
    err = err ? err : compute_motion(prev, cur, (int)w, (int)h, stride, stride, &cpu, 0);
    const double blocks = (err == 0) ? fm_block_score(cur, prev, w, h) : cpu;
    free(cur);
    free(prev);
    mu_assert("compute_motion() failed", err == 0);
    mu_assert("a per-block sum gives the CPU's score: the fixture cannot tell the orders apart",
              blocks != cpu);
    return nullptr;
}

} // namespace

mu_message_t run_tests(void)
{
    mu_run_test(test_reflect101_is_the_cpu_fold);
    mu_run_test(test_taps_are_the_cpu_filters);
    mu_run_test(test_blur_is_the_cpu_convolution);
    mu_run_test(test_row_sums_are_compute_motion);
    mu_run_test(test_scale1_row_sums_are_compute_motion);
    mu_run_test(test_frame_score_is_the_cpu_extractor);
    mu_run_test(test_block_reduction_is_detected);
    return nullptr;
}
