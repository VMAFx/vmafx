/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The Metal IOSurface import of the frames VideoToolbox decodes to
 * (T-METAL-FFMPEG-FILTER-BIPLANAR-IMPORT-2026-10-05, ADR-1679). The FFmpeg
 * `libvmaf_metal` filter hands libvmaf the IOSurface of every NV12 or P010
 * frame and imports planes 0, 1 and 2; the import de-interleaves the CbCr
 * plane and shifts P010's MSB-aligned samples. Before ADR-1679 the filter
 * imported plane 0 only, vmaf_metal_read_imported_pictures() refused the
 * half-imported frame with -EINVAL, and a plane-1 import would have copied
 * the interleaved bytes as the Cb plane.
 *
 * On an Apple device each case writes a planar fixture into a CVPixelBuffer
 * of the format VideoToolbox produces (IOSurface-backed, as FFmpeg's decoder
 * requests it), imports the three planes of the reference and the distorted
 * frame through the public API, and checks:
 * - the imported planes equal the planar fixture sample for sample;
 * - psnr_y / psnr_cb / psnr_cr of the frames read through
 *   vmaf_metal_read_imported_pictures() equal the CPU's on the planar
 *   fixture (`==`; the extractor is the CPU's "psnr" in both contexts).
 * The self-test (every host) builds the bi-planar layout in memory and reads
 * it with core/src/metal/iosurface_layout.h, the code the import runs, so the
 * fixture and the scoring comparison are checked before a tester runs them.
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "metal_twin.h"

#include "libvmaf/picture.h"
#include "metal/iosurface_layout.h"

#ifndef VMAF_METAL_TWIN_SELFTEST
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>

#include "metal/import.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define FRAME_W 64u
#define FRAME_H 48u

/* Planar fixture: a gradient per plane; the distorted frame adds a
 * deterministic offset in [-3, 3] scaled to the bit depth, clamped. */
static unsigned fixture_value(unsigned plane, unsigned y, unsigned x, int is_ref, unsigned bpc)
{
    const long max = (1L << bpc) - 1L;
    const long scale = 1L << (bpc - 8u);
    long v = ((long)((x * 3u + y * 5u + plane * 40u) % 200u) + 20L) * scale;
    if (!is_ref) {
        v += ((long)((x * 7u + y * 13u + plane) % 7u) - 3L) * scale;
    }
    if (v < 0) {
        v = 0;
    }
    return (unsigned)((v > max) ? max : v);
}

/* Store sample `idx` of a row: one byte at 8 bits, else one uint16. */
static void put_raw(uint8_t *row, unsigned idx, unsigned bpc, unsigned v)
{
    if (bpc > 8u) {
        const uint16_t s = (uint16_t)v;
        memcpy(row + (size_t)idx * 2u, &s, sizeof(s));
    } else {
        row[idx] = (uint8_t)v;
    }
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned y, unsigned x, unsigned v)
{
    uint8_t *row = (uint8_t *)pic->data[plane] + (size_t)y * (size_t)pic->stride[plane];
    put_raw(row, x, pic->bpc, v);
}

static unsigned get_sample(const VmafPicture *pic, unsigned plane, unsigned y, unsigned x)
{
    const uint8_t *row = (const uint8_t *)pic->data[plane] + (size_t)y * (size_t)pic->stride[plane];
    if (pic->bpc > 8u) {
        uint16_t s = 0u;
        memcpy(&s, row + (size_t)x * 2u, sizeof(s));
        return s;
    }
    return row[x];
}

static int planar_fixture(VmafPicture *pic, int is_ref, unsigned bpc)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, FRAME_W, FRAME_H);
    if (err) {
        return err;
    }
    for (unsigned p = 0u; p < 3u; p++) {
        for (unsigned y = 0u; y < pic->h[p]; y++) {
            for (unsigned x = 0u; x < pic->w[p]; x++) {
                put_sample(pic, p, y, x, fixture_value(p, y, x, is_ref, bpc));
            }
        }
    }
    return 0;
}

static int plane_equal(const VmafPicture *a, const VmafPicture *b, unsigned p)
{
    if (a->w[p] != b->w[p] || a->h[p] != b->h[p]) {
        return 0;
    }
    for (unsigned y = 0u; y < a->h[p]; y++) {
        for (unsigned x = 0u; x < a->w[p]; x++) {
            if (get_sample(a, p, y, x) != get_sample(b, p, y, x)) {
                return 0;
            }
        }
    }
    return 1;
}

static int planes_equal(const VmafPicture *a, const VmafPicture *b)
{
    return plane_equal(a, b, 0u) && plane_equal(a, b, 1u) && plane_equal(a, b, 2u);
}

/* Write one planar picture as a bi-planar NV12 / P010 frame: plane 0 the
 * luma rows, plane 1 Cb and Cr interleaved, P010 samples in the top bits. */
static void write_biplanar(const VmafPicture *pic, uint8_t *const dst[2], const size_t stride[2])
{
    const unsigned shift = (pic->bpc > 8u) ? 16u - pic->bpc : 0u;
    for (unsigned y = 0u; y < pic->h[0]; y++) {
        uint8_t *row = dst[0] + (size_t)y * stride[0];
        for (unsigned x = 0u; x < pic->w[0]; x++) {
            put_raw(row, x, pic->bpc, get_sample(pic, 0u, y, x) << shift);
        }
    }
    for (unsigned y = 0u; y < pic->h[1]; y++) {
        uint8_t *row = dst[1] + (size_t)y * stride[1];
        for (unsigned x = 0u; x < pic->w[1]; x++) {
            put_raw(row, 2u * x, pic->bpc, get_sample(pic, 1u, y, x) << shift);
            put_raw(row, 2u * x + 1u, pic->bpc, get_sample(pic, 2u, y, x) << shift);
        }
    }
}

typedef struct PsnrScores {
    double y, cb, cr;
} PsnrScores;

static int psnr_scores(VmafContext *vmaf, PsnrScores *out)
{
    int err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    err = err ? err : vmaf_feature_score_at_index(vmaf, "psnr_y", &out->y, 0);
    err = err ? err : vmaf_feature_score_at_index(vmaf, "psnr_cb", &out->cb, 0);
    err = err ? err : vmaf_feature_score_at_index(vmaf, "psnr_cr", &out->cr, 0);
    return err;
}

/* The CPU's psnr of the planar pair (consumes both pictures). */
static int cpu_psnr(VmafPicture *ref, VmafPicture *dist, PsnrScores *out)
{
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 0};
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = vmaf_use_feature(vmaf, "psnr", NULL);
    err = err ? err : vmaf_read_pictures(vmaf, ref, dist, 0);
    err = err ? err : psnr_scores(vmaf, out);
    const int close_err = vmaf_close(vmaf);
    return err ? err : close_err;
}

#ifdef VMAF_METAL_TWIN_SELFTEST
/* Self-test: the bi-planar frame in memory, read back with the import's
 * layout code into a planar picture. */
static int import_frame(const VmafPicture *planar, uint32_t fourcc, VmafPicture *out)
{
    const VmafMetalSurfaceFormat *fmt = vmaf_metal_surface_format(fourcc);
    const size_t bytes = (planar->bpc > 8u) ? 2u : 1u;
    const size_t stride[2] = {FRAME_W * bytes + 32u, FRAME_W * bytes + 32u};
    uint8_t *buf[2] = {calloc(FRAME_H, stride[0]), calloc(FRAME_H / 2u, stride[1])};
    int err = (fmt == NULL || !buf[0] || !buf[1]) ? -ENOMEM : 0;
    err = err ? err : vmaf_picture_alloc(out, VMAF_PIX_FMT_YUV420P, planar->bpc, FRAME_W, FRAME_H);
    if (!err) {
        write_biplanar(planar, buf, stride);
    }
    for (unsigned p = 0u; !err && p < 3u; p++) {
        const unsigned sp = vmaf_metal_surface_src_plane(fmt, p);
        const VmafMetalSurfacePlane geo = {out->w[p], out->h[p], (sp ? 2u : 1u) * bytes,
                                           stride[sp]};
        VmafMetalPlaneRead rd;
        err = vmaf_metal_plane_read_plan(fmt, 2u, p, planar->bpc, &geo, out->w[p], out->h[p], &rd);
        if (!err) {
            vmaf_metal_read_plane(out->data[p], (size_t)out->stride[p], buf[sp], stride[sp],
                                  out->w[p], out->h[p], &rd);
        }
    }
    free(buf[0]);
    free(buf[1]);
    return err;
}

/* Consumes `ref` and `dist`, as the device build's imported_psnr() does: the
 * planar pair is read into the imported pictures and released here
 * (T-METAL-IOSURFACE-SELFTEST-LEAK-2026-10-06). */
static int imported_psnr(VmafPicture *ref, VmafPicture *dist, uint32_t fourcc, PsnrScores *out)
{
    VmafPicture iref = {0};
    VmafPicture idist = {0};
    int err = import_frame(ref, fourcc, &iref);
    err = err ? err : import_frame(dist, fourcc, &idist);
    err = err ? err : (planes_equal(ref, &iref) && planes_equal(dist, &idist)) ? 0 : -EDOM;
    const int ref_unref = vmaf_picture_unref(ref);
    const int dist_unref = vmaf_picture_unref(dist);
    err = err ? err : (ref_unref ? ref_unref : dist_unref);
    if (err) {
        (void)vmaf_picture_unref(&iref);
        (void)vmaf_picture_unref(&idist);
        return err;
    }
    return cpu_psnr(&iref, &idist, out);
}
#else
/* An IOSurface-backed CVPixelBuffer of `fourcc` holding `planar`, as
 * VideoToolbox hands it to FFmpeg. */
static CVPixelBufferRef make_frame(const VmafPicture *planar, uint32_t fourcc)
{
    CFDictionaryRef none =
        CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {none};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pb = NULL;
    const CVReturn ret =
        CVPixelBufferCreate(kCFAllocatorDefault, FRAME_W, FRAME_H, (OSType)fourcc, attrs, &pb);
    CFRelease(attrs);
    CFRelease(none);
    if (ret != kCVReturnSuccess || pb == NULL) {
        return NULL;
    }
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(pb);
        return NULL;
    }
    uint8_t *const dst[2] = {(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0),
                             (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1)};
    const size_t stride[2] = {CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
                              CVPixelBufferGetBytesPerRowOfPlane(pb, 1)};
    write_biplanar(planar, dst, stride);
    (void)CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

/* Import the three planes of `pb` as the reference or distorted frame 0. */
static int import_planes(VmafMetalState *state, CVPixelBufferRef pb, unsigned bpc, int is_ref)
{
    IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
    if (surf == NULL) {
        return -ENOENT;
    }
    for (unsigned p = 0u; p < 3u; p++) {
        const int err =
            vmaf_metal_picture_import(state, (uintptr_t)surf, p, FRAME_W, FRAME_H, bpc, is_ref, 0u);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* The imported pair as the import ring built it, against the fixture. */
static int check_imported_planes(VmafMetalState *state, const VmafPicture *ref,
                                 const VmafPicture *dist)
{
    VmafPicture iref = {0};
    VmafPicture idist = {0};
    int err = vmaf_metal_state_build_pictures(state, 0u, &iref, &idist);
    if (!err && !(planes_equal(ref, &iref) && planes_equal(dist, &idist))) {
        err = -EDOM;
    }
    (void)vmaf_picture_unref(&iref);
    (void)vmaf_picture_unref(&idist);
    return err;
}

/* Import ref and dist through the filter's calls; `score` reads them
 * through vmaf_metal_read_imported_pictures(), else the planes are
 * compared with the fixture. */
static int import_pair(VmafMetalState *state, CVPixelBufferRef pr, CVPixelBufferRef pd,
                       const VmafPicture *ref, const VmafPicture *dist, VmafContext *score)
{
    int err = import_planes(state, pr, ref->bpc, 1);
    err = err ? err : import_planes(state, pd, dist->bpc, 0);
    if (err) {
        return err;
    }
    return score ? vmaf_metal_read_imported_pictures(score, 0u) :
                   check_imported_planes(state, ref, dist);
}

static int metal_import_run(VmafPicture *ref, VmafPicture *dist, uint32_t fourcc, PsnrScores *out)
{
    VmafMetalState *state = NULL;
    const VmafMetalExternalHandles handles = {.device = 0u, .command_queue = 0u};
    int err = vmaf_metal_state_init_external(&state, handles);
    if (err) {
        return err;
    }
    CVPixelBufferRef pr = make_frame(ref, fourcc);
    CVPixelBufferRef pd = make_frame(dist, fourcc);
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 0};
    err = (pr && pd) ? import_pair(state, pr, pd, ref, dist, NULL) : -ENOMEM;
    err = err ? err : vmaf_init(&vmaf, cfg);
    err = err ? err : vmaf_metal_import_state(vmaf, state);
    err = err ? err : vmaf_use_feature(vmaf, "psnr", NULL);
    err = err ? err : import_pair(state, pr, pd, ref, dist, vmaf);
    err = err ? err : psnr_scores(vmaf, out);
    if (vmaf) {
        const int close_err = vmaf_close(vmaf);
        err = err ? err : close_err;
    }
    if (pr) {
        CVPixelBufferRelease(pr);
    }
    if (pd) {
        CVPixelBufferRelease(pd);
    }
    vmaf_metal_state_free(&state);
    return err;
}

static int imported_psnr(VmafPicture *ref, VmafPicture *dist, uint32_t fourcc, PsnrScores *out)
{
    const int err = metal_import_run(ref, dist, fourcc, out);
    (void)vmaf_picture_unref(ref);
    (void)vmaf_picture_unref(dist);
    return err;
}
#endif

/* Two planar pairs of the fixture: one for the CPU, one to import. */
static int fixture_pairs(VmafPicture ref[2], VmafPicture dist[2], unsigned bpc)
{
    int err = 0;
    for (int i = 0; i < 2 && !err; i++) {
        err = planar_fixture(&ref[i], 1, bpc);
        err = err ? err : planar_fixture(&dist[i], 0, bpc);
    }
    return err;
}

static int scores_equal(const PsnrScores *a, const PsnrScores *b)
{
    return a->y == b->y && a->cb == b->cb && a->cr == b->cr;
}

/* Import the NV12 / P010 frame pair and compare its psnr with the CPU's on
 * the planar pair, `==` on all three planes. */
static char *import_scores_like_planar(uint32_t fourcc, unsigned bpc)
{
    if (!metal_twin_have_device()) {
        return NULL;
    }
    VmafPicture ref[2] = {{0}};
    VmafPicture dist[2] = {{0}};
    mu_assert("planar fixtures", fixture_pairs(ref, dist, bpc) == 0);
    PsnrScores cpu = {0};
    PsnrScores imported = {0};
    mu_assert("CPU psnr of the planar pair", cpu_psnr(&ref[0], &dist[0], &cpu) == 0);
    mu_assert("import of all three planes and read of the frame",
              imported_psnr(&ref[1], &dist[1], fourcc, &imported) == 0);
    mu_assert("psnr_y / psnr_cb / psnr_cr of the imported frame are the CPU's",
              scores_equal(&imported, &cpu));
    return NULL;
}

static char *test_iosurface_nv12_import_exact(void)
{
    return import_scores_like_planar(VMAF_METAL_FOURCC('4', '2', '0', 'v'), 8u);
}

static char *test_iosurface_nv12_full_range_import_exact(void)
{
    return import_scores_like_planar(VMAF_METAL_FOURCC('4', '2', '0', 'f'), 8u);
}

static char *test_iosurface_p010_import_exact(void)
{
    return import_scores_like_planar(VMAF_METAL_FOURCC('x', '4', '2', '0'), 10u);
}

char *run_tests(void)
{
    metal_run_case(test_iosurface_nv12_import_exact);
    metal_run_case(test_iosurface_nv12_full_range_import_exact);
    metal_run_case(test_iosurface_p010_import_exact);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
