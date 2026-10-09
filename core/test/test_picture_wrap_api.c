/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The contract of vmaf_picture_wrap() in the public libvmaf (ADR-2949), past
 * upstream's test (test_picture_wrap_integration.c):
 *
 *  - positive: a wrapped odd-size 10-bit 4:2:0 pair scores bit for bit what
 *    the same samples score through vmaf_picture_alloc() (VMAF frame by frame
 *    and pooled, psnr_cb and psnr_cr for the chroma planes); the release runs
 *    once per picture, by the end of vmaf_close(), with the
 *    wrapped geometry and planes and cleared internal fields;
 *  - negative: what upstream accepts and the fork refuses (a chroma stride of
 *    upstream's rounded-down width, a NULL plane, a size of 0, a negative
 *    stride, an unknown format or depth) is -EINVAL, leaves the picture
 *    untouched and calls no release;
 *  - boundary: 8 and 16 bits, a 1x1 picture, YUV400P without chroma planes,
 *    no release callback.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define PIC_W 97u /* odd: chroma extents are rounded up (ADR-1483) */
#define PIC_H 75u
#define PIC_BPC 10u
#define N_FRAMES 3u
#define CHROMA_W ((PIC_W + 1u) / 2u)
#define CHROMA_H ((PIC_H + 1u) / 2u)
#define FRAME_SAMPLES ((PIC_W * PIC_H) + (2u * CHROMA_W * CHROMA_H))

/* One frame's samples, planes tight: Y, then U, then V. */
typedef struct Frame {
    uint16_t s[FRAME_SAMPLES];
} Frame;

#define SEEN_MAX 8u

/* What the release callback saw: the last picture, and the luma plane of
 * each call. */
typedef struct Release {
    unsigned calls;
    VmafPicture seen;
    const void *luma[SEEN_MAX];
} Release;

static int record_release(VmafPicture *pic, void *cookie)
{
    Release *const r = cookie;
    if (r->calls < SEEN_MAX) {
        r->luma[r->calls] = pic->data[0];
    }
    r->calls++;
    r->seen = *pic;
    return 0;
}

/* Each frame's luma plane was released exactly once. */
static int each_released_once(const Release *r, const Frame *frames, unsigned n)
{
    for (unsigned f = 0; f < n; f++) {
        unsigned hits = 0;
        for (unsigned c = 0; c < r->calls && c < SEEN_MAX; c++) {
            hits += r->luma[c] == (const void *)frames[f].s;
        }
        if (hits != 1u) {
            return 0;
        }
    }
    return 1;
}

/* xorshift32: deterministic texture without rand() (banned, principles.md). */
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_frame(Frame *f, unsigned index, int distorted)
{
    uint32_t seed = 0x9e3779b9u + index;
    for (unsigned i = 0; i < FRAME_SAMPLES; i++) {
        const unsigned base = ((i * 37u) + (index * 101u)) % 1024u;
        const unsigned noise = distorted ? next_random(&seed) % 41u : 0u;
        f->s[i] = (uint16_t)((base + noise) % 1024u);
    }
}

/* The wrapped description of a Frame (tight 16-bit planes). */
static VmafPictureWrapped wrap_of(Frame *f, Release *release)
{
    VmafPictureWrapped w;
    memset(&w, 0, sizeof(w));
    w.pix_fmt = VMAF_PIX_FMT_YUV420P;
    w.bpc = PIC_BPC;
    w.w = PIC_W;
    w.h = PIC_H;
    w.data[0] = f->s;
    w.data[1] = f->s + ((size_t)PIC_W * PIC_H);
    w.data[2] = f->s + ((size_t)PIC_W * PIC_H) + ((size_t)CHROMA_W * CHROMA_H);
    w.stride[0] = (ptrdiff_t)(PIC_W * sizeof(uint16_t));
    w.stride[1] = w.stride[2] = (ptrdiff_t)(CHROMA_W * sizeof(uint16_t));
    w.cookie = release;
    w.release_picture = release ? record_release : NULL;
    return w;
}

/* The same samples in a picture of vmaf_picture_alloc(). */
static int alloc_copy(Frame *f, VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, PIC_BPC, PIC_W, PIC_H);
    if (err) {
        return err;
    }
    const VmafPictureWrapped w = wrap_of(f, NULL);
    for (unsigned p = 0; p < 3u; p++) {
        const size_t row = (size_t)pic->w[p] * sizeof(uint16_t);
        for (unsigned y = 0; y < pic->h[p]; y++) {
            memcpy((uint8_t *)pic->data[p] + ((size_t)y * (size_t)pic->stride[p]),
                   (const uint8_t *)w.data[p] + ((size_t)y * (size_t)w.stride[p]), row);
        }
    }
    return 0;
}

/* One pair of frame `i`, wrapped or copied. */
static int read_pair(VmafContext *vmaf, Frame *ref, Frame *dist, unsigned i, Release *release)
{
    VmafPicture r;
    VmafPicture d;
    int err = release ? vmaf_picture_wrap(&r, wrap_of(ref, release)) : alloc_copy(ref, &r);
    if (err) {
        return err;
    }
    err = release ? vmaf_picture_wrap(&d, wrap_of(dist, release)) : alloc_copy(dist, &d);
    if (err) {
        (void)vmaf_picture_unref(&r);
        return err;
    }
    return vmaf_read_pictures(vmaf, &r, &d, i);
}

/* Scores compared: VMAF per frame, pooled VMAF, then psnr_cb and psnr_cr per
 * frame (vmaf_v0.6.1 reads luma only; psnr covers the chroma planes). */
#define N_SCORES ((N_FRAMES * 3u) + 1u)

static int collect_scores(VmafContext *vmaf, VmafModel *model, double out[N_SCORES])
{
    int err = 0;
    for (unsigned i = 0; i < N_FRAMES && !err; i++) {
        err = vmaf_score_at_index(vmaf, model, &out[i], i);
    }
    if (!err) {
        err =
            vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &out[N_FRAMES], 0, N_FRAMES - 1u);
    }
    double *const chroma = &out[N_FRAMES + 1u];
    for (unsigned i = 0; i < N_FRAMES && !err; i++) {
        err = vmaf_feature_score_at_index(vmaf, "psnr_cb", &chroma[(size_t)2u * i], i);
        if (!err) {
            err = vmaf_feature_score_at_index(vmaf, "psnr_cr", &chroma[((size_t)2u * i) + 1u], i);
        }
    }
    return err;
}

/* The scores of the frames, through wrapped pictures when `release` is set,
 * else through copies. */
static int score(Frame *ref, Frame *dist, Release *release, double out[N_SCORES])
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;
    VmafModelConfig model_cfg;
    memset(&model_cfg, 0, sizeof(model_cfg));
    int err = vmaf_init(&vmaf, cfg);
    if (!err) {
        err = vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1");
    }
    if (!err) {
        err = vmaf_use_features_from_model(vmaf, model);
    }
    if (!err) {
        err = vmaf_use_feature(vmaf, "psnr", NULL);
    }
    for (unsigned i = 0; i < N_FRAMES && !err; i++) {
        err = read_pair(vmaf, &ref[i], &dist[i], i, release);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    if (!err) {
        err = collect_scores(vmaf, model, out);
    }
    if (vmaf) {
        const int close_err = vmaf_close(vmaf);
        err = err ? err : close_err;
    }
    if (model) {
        vmaf_model_destroy(model);
    }
    return err;
}

/* Bit-for-bit equality of two score arrays (no floating-point compare). */
static int same_bits(const double *a, const double *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t x = 0;
        uint64_t y = 0;
        memcpy(&x, &a[i], sizeof(x));
        memcpy(&y, &b[i], sizeof(y));
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

/* The last release saw the wrapped geometry and strides, with cleared
 * internal fields. */
static int release_view_ok(const VmafPicture *seen)
{
    return seen->pix_fmt == VMAF_PIX_FMT_YUV420P && seen->bpc == PIC_BPC && seen->w[0] == PIC_W &&
           seen->h[0] == PIC_H && seen->w[1] == CHROMA_W && seen->h[1] == CHROMA_H &&
           seen->stride[1] == (ptrdiff_t)CHROMA_W * (ptrdiff_t)sizeof(uint16_t) &&
           seen->ref == NULL && seen->priv == NULL;
}

static char *test_wrapped_scores_equal_allocated(void)
{
    Frame *const frames = calloc((size_t)2u * N_FRAMES, sizeof(*frames));
    mu_assert("frames", frames != NULL);
    for (unsigned i = 0; i < N_FRAMES; i++) {
        fill_frame(&frames[i], i, 0);
        fill_frame(&frames[N_FRAMES + i], i, 1);
    }
    double copied[N_SCORES] = {0};
    double wrapped[N_SCORES] = {0};
    Release release = {0};
    const int a = score(frames, frames + N_FRAMES, NULL, copied);
    const int b = score(frames, frames + N_FRAMES, &release, wrapped);
    const int once = each_released_once(&release, frames, 2u * N_FRAMES);
    free(frames);
    mu_assert("scoring copies", a == 0);
    mu_assert("scoring wrapped pictures", b == 0);
    mu_assert("wrapped scores equal the copies' bit for bit", same_bits(copied, wrapped, N_SCORES));
    mu_assert("one release per wrapped picture, by vmaf_close", release.calls == 2u * N_FRAMES);
    mu_assert("each wrapped plane is released once", once);
    mu_assert("the release sees the wrapped view", release_view_ok(&release.seen));
    return NULL;
}

/* A refused wrap: -EINVAL, `pic` untouched, no release. */
static int refused(VmafPictureWrapped w)
{
    VmafPicture pic;
    VmafPicture before;
    memset(&pic, 0x5a, sizeof(pic));
    memcpy(&before, &pic, sizeof(before));
    const Release *const release = w.cookie;
    const int err = vmaf_picture_wrap(&pic, w);
    return err == -EINVAL && memcmp(&pic, &before, sizeof(pic)) == 0 && release->calls == 0u;
}

/* Planes upstream accepts and the fork refuses. */
static char *test_plane_refusals(void)
{
    static Frame f;
    Release release = {0};
    const VmafPictureWrapped good = wrap_of(&f, &release);
    mu_assert("NULL picture", vmaf_picture_wrap(NULL, good) == -EINVAL);
    VmafPictureWrapped w = good;
    w.stride[1] = (ptrdiff_t)(PIC_W / 2u) * (ptrdiff_t)sizeof(uint16_t);
    mu_assert("upstream's rounded-down chroma stride", refused(w));
    w = good;
    w.data[2] = NULL;
    mu_assert("NULL chroma plane", refused(w));
    w = good;
    w.data[0] = NULL;
    mu_assert("NULL luma plane", refused(w));
    w = good;
    w.stride[0] = -w.stride[0];
    mu_assert("negative stride", refused(w));
    return NULL;
}

/* Sizes, formats and depths both refuse. */
static char *test_format_refusals(void)
{
    static Frame f;
    Release release = {0};
    const VmafPictureWrapped good = wrap_of(&f, &release);
    VmafPictureWrapped w = good;
    w.w = 0;
    mu_assert("width 0", refused(w));
    w = good;
    w.h = 0;
    mu_assert("height 0", refused(w));
    w = good;
    w.pix_fmt = VMAF_PIX_FMT_UNKNOWN;
    mu_assert("unknown format", refused(w));
    w = good;
    w.bpc = 7u;
    mu_assert("7 bits", refused(w));
    w = good;
    w.bpc = 17u;
    mu_assert("17 bits", refused(w));
    return NULL;
}

/* Wrap `w`, check the luma extent and unref: the release runs once. */
static int wraps_once(VmafPictureWrapped w, unsigned luma_w)
{
    Release *const release = w.cookie;
    VmafPicture pic;
    if (vmaf_picture_wrap(&pic, w) != 0) {
        return 0;
    }
    const int shape = pic.w[0] == luma_w && pic.ref != NULL && release->calls == 0u;
    return vmaf_picture_unref(&pic) == 0 && shape && release->calls == 1u;
}

static char *test_boundaries(void)
{
    static uint16_t samples[4] = {0};
    Release r8 = {0};
    Release r16 = {0};
    Release r400 = {0};
    VmafPictureWrapped w;
    memset(&w, 0, sizeof(w));
    w.pix_fmt = VMAF_PIX_FMT_YUV444P;
    w.bpc = 8u;
    w.w = 1u;
    w.h = 1u;
    w.data[0] = w.data[1] = w.data[2] = samples;
    w.stride[0] = w.stride[1] = w.stride[2] = 1;
    w.cookie = &r8;
    w.release_picture = record_release;
    mu_assert("1x1, 8 bits", wraps_once(w, 1u));
    w.bpc = 16u;
    w.stride[0] = w.stride[1] = w.stride[2] = 2;
    w.cookie = &r16;
    mu_assert("1x1, 16 bits", wraps_once(w, 1u));
    w.pix_fmt = VMAF_PIX_FMT_YUV400P;
    w.data[1] = w.data[2] = NULL;
    w.stride[1] = w.stride[2] = 0;
    w.cookie = &r400;
    mu_assert("YUV400P without chroma planes", wraps_once(w, 1u));
    w.release_picture = NULL;
    w.cookie = NULL;
    VmafPicture pic;
    mu_assert("no release callback", vmaf_picture_wrap(&pic, w) == 0);
    mu_assert("unref without a release", vmaf_picture_unref(&pic) == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_wrapped_scores_equal_allocated);
    mu_run_test(test_plane_refusals);
    mu_run_test(test_format_refusals);
    mu_run_test(test_boundaries);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
