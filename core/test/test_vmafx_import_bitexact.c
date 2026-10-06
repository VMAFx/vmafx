/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Imported frames score bit for bit as the same frames created on the host
 * (RC4 WP3 common lane, ADR-1829 exit evidence on the CPU device).
 *
 * For the Netflix 576x324 golden pair, both 1080p checkerboard pairs, the
 * 10-bit sparks pair and synthetic 4K frames, one context scores frames the
 * host planes are borrowed for (vmafx_frame_wrap_host) and one scores the
 * same frames imported as NV12 (8-bit) or P010 (10-bit), and as P016 for the
 * 16-bit form of the sparks pair. Compared bit for bit: every feature the
 * collector holds at every frame and the model score of every frame. The
 * conversion counter must count every semi-planar import and the host-copy
 * counter must stay 0 (no host copy of imported pixels, D8).
 *
 * One import, many contexts (PR #2185): a frame pair imported once is
 * submitted to two contexts with different models; each context's scores
 * equal those of a run of its own, no second import happens, and the release
 * fence is signalled only after the last reader of either context.
 *
 * The YUV files live in python/test/resource/yuv (VMAFX_TEST_YUV_DIR); the
 * fixture cases are skipped when they are missing, the synthetic ones run
 * always. Failing first: vmafx_frame_import() does not exist on the WP2 base;
 * measured with planted defects on this branch: a P010 shift of 5 and a
 * swapped Cb / Cr de-interleave each fail the feature comparison.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feature/feature_collector.h"
#include "libvmaf_priv.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_fixture_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define MODEL "vmaf_v1.0.16_3d0h"

/* Values compared and frames imported, over the whole run. */
static unsigned long compared;
static uint64_t imported;

/* How the import session lays out the frames. */
typedef struct Form {
    uint32_t pix_fmt; /* NV12 .. P416, Y210, Y410 */
    uint32_t bpc;
    unsigned shift; /* left shift of the samples in the semi-planar buffer */
} Form;

/* Two frames of deterministic content at `bpc`. */
static bool clip_synthetic(VtClip *clip, uint32_t w, uint32_t h, uint32_t bpc)
{
    clip->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, bpc, w, h);
    clip->n_frames = 2u;
    const size_t bytes = vt_frame_bytes(&clip->desc) * clip->n_frames;
    clip->ref = malloc(bytes);
    clip->dist = malloc(bytes);
    if (!clip->ref || !clip->dist) {
        return false;
    }
    for (unsigned i = 0; i < clip->n_frames; i++) {
        const size_t at = vt_frame_bytes(&clip->desc) * i;
        vt_fill(&clip->desc, clip->ref + at, i);
        vt_fill(&clip->desc, clip->dist + at, i + 7u);
    }
    return true;
}

/* ---- Sessions ----------------------------------------------------------------------- */

static VmafxContext *model_context(VmafxModel *model)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_model(context, model, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* The host session: frames borrowed from the clip's planar buffers. */
static VmafxContext *run_host(const VtClip *clip, VmafxModel *model)
{
    VmafxContext *const context = model_context(model);
    const size_t frame = vt_frame_bytes(&clip->desc);
    bool ok = context != NULL;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        VmafxFrame *ref = vt_wrap_frame(&clip->desc, clip->ref + i * frame, NULL);
        VmafxFrame *dist = vt_wrap_frame(&clip->desc, clip->dist + i * frame, NULL);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* One frame of the clip imported in `form`, through the semi-planar buffer
 * `buf` (which must outlive the frame). */
static VmafxFrame *import_frame(const VtClip *clip, const Form *form, const uint8_t *planar,
                                uint8_t *buf)
{
    VmafxFrameImport imp;
    if (vt_is_msb(form->pix_fmt)) {
        memcpy(buf, planar, vt_frame_bytes(&clip->desc));
        vt_shift_up(&clip->desc, buf, 16u - clip->desc.bpc);
        imp = vt_import_planar_words(&clip->desc, form->pix_fmt, buf);
    } else if (vt_is_packed(form->pix_fmt)) {
        vt_to_packed(&clip->desc, planar, form->pix_fmt, buf);
        imp = vt_import_packed(&clip->desc, form->pix_fmt, buf);
    } else {
        vt_to_semiplanar(&clip->desc, planar, form->shift, buf);
        imp = vt_import_semiplanar(&clip->desc, form->pix_fmt, form->bpc, buf);
    }
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(NULL, &imp, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    imported++;
    return frame;
}

/* The import session; `bufs` holds 2 * n_frames semi-planar frames. */
static VmafxContext *run_import(const VtClip *clip, const Form *form, VmafxModel *model,
                                uint8_t *bufs)
{
    VmafxContext *const context = model_context(model);
    const size_t frame = vt_import_bytes(&clip->desc);
    const size_t planar = vt_frame_bytes(&clip->desc);
    bool ok = context != NULL;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        uint8_t *const buf = bufs + 2u * (size_t)i * frame;
        VmafxFrame *ref = import_frame(clip, form, clip->ref + (size_t)i * planar, buf);
        VmafxFrame *dist = import_frame(clip, form, clip->dist + (size_t)i * planar, buf + frame);
        ok = ref && dist && vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* ---- Comparison ---------------------------------------------------------------------- */

/* Every feature of `a`'s collector at every frame, and the model score, are
 * the same bits in `b`. */
static char *compare_contexts(VmafxContext *a, VmafxContext *b, const VmafxModel *model, unsigned n)
{
    const VmafFeatureCollector *fa = vmaf_feature_collector_get(vmafx_context_libvmaf_handle(a));
    const VmafFeatureCollector *fb = vmaf_feature_collector_get(vmafx_context_libvmaf_handle(b));
    mu_assert("collectors", fa && fb && fa->cnt == fb->cnt && fa->cnt > 0);
    for (unsigned f = 0; f < fa->cnt; f++) {
        const char *name = fa->feature_vector[f]->name;
        for (unsigned i = 0; i < n; i++) {
            VmafxScore sa = VMAFX_SCORE_INIT;
            VmafxScore sb = VMAFX_SCORE_INIT;
            const VmafxStatus ra = vmafx_feature_score(a, name, i, &sa, NULL);
            const VmafxStatus rb = vmafx_feature_score(b, name, i, &sb, NULL);
            mu_assert("feature status", ra == rb);
            mu_assert("feature bits", ra != VMAFX_OK || vt_same_bits(sa.value, sb.value));
            compared++;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        VmafxScore sa = VMAFX_SCORE_INIT;
        VmafxScore sb = VMAFX_SCORE_INIT;
        mu_assert("model score", vmafx_score_frame(a, model, i, &sa, NULL) == VMAFX_OK &&
                                     vmafx_score_frame(b, model, i, &sb, NULL) == VMAFX_OK &&
                                     vt_same_bits(sa.value, sb.value));
        compared++;
    }
    return NULL;
}

/* One host session and one import session of `clip`, compared. */
static char *compare_sessions(const VtClip *clip, const VtClip *host_clip, const Form *form,
                              VmafxModel *model, uint8_t *bufs)
{
    VmafxContext *const host = run_host(host_clip, model);
    VmafxContext *const imp = run_import(clip, form, model, bufs);
    char *const msg = host && imp ? compare_contexts(host, imp, model, clip->n_frames) : "sessions";
    const bool destroyed = (!host || vmafx_context_destroy(host, NULL) == VMAFX_OK) &&
                           (!imp || vmafx_context_destroy(imp, NULL) == VMAFX_OK);
    mu_assert_msg(msg);
    mu_assert("destroy", destroyed);
    return NULL;
}

static char *compare_case(const VtClip *clip, const VtClip *host_clip, const Form *form)
{
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    uint8_t *const bufs = malloc(2u * (size_t)clip->n_frames * vt_import_bytes(&clip->desc));
    char *const msg = bufs ? compare_sessions(clip, host_clip, form, model, bufs) : "buffers";
    free(bufs);
    vmafx_model_unref(model);
    return msg;
}

static char *test_fixture_imports(void)
{
    static const Form nv12 = {VMAFX_PIXEL_FORMAT_NV12, 8u, 0u};
    static const Form p010 = {VMAFX_PIXEL_FORMAT_P010, 10u, 6u};
    unsigned cases = 0;
    for (size_t i = 0; i < VT_N_INPUTS; i++) {
        VtClip clip;
        const bool present = vt_clip_open(&clip, &vt_inputs[i]);
        char *const msg =
            present ? compare_case(&clip, &clip, clip.desc.bpc == 8u ? &nv12 : &p010) : NULL;
        vt_clip_close(&clip);
        mu_assert_msg(msg);
        cases += present ? 1u : 0u;
    }
    (void)fprintf(stderr, "[%u fixture cases] ", cases);
    mu_assert("every input present or none (partial fixture set)", cases == 0 || cases == 4u);
    return NULL;
}

/* P016: the sparks pair scaled to 16 bits, against the same 16-bit planes
 * created on the host. */
static char *test_fixture_p016(void)
{
    static const Form p016 = {VMAFX_PIXEL_FORMAT_P016, 16u, 0u};
    VtClip clip;
    if (!vt_clip_open(&clip, &vt_inputs[3])) {
        vt_clip_close(&clip);
        return NULL;
    }
    const VmafxFrameDesc d16 = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 16, clip.desc.w, clip.desc.h);
    const size_t frame = vt_frame_bytes(&clip.desc);
    for (unsigned i = 0; i < clip.n_frames; i++) {
        vt_shift_up(&d16, clip.ref + (size_t)i * frame, 6u);
        vt_shift_up(&d16, clip.dist + (size_t)i * frame, 6u);
    }
    clip.desc = d16;
    char *const msg = compare_case(&clip, &clip, &p016);
    vt_clip_close(&clip);
    return msg;
}

/* 4:2:2 and 4:4:4 imports (ADR-2133): a fixture's chroma repeated into the
 * wider layouts, imported semi-planar (NVxx at 8 bits, Pxxx at 10 and 16) and
 * packed (Y210 as 4:2:2, Y410 as 4:4:4 at 10 bits), against the same planes
 * created on the host. */
static char *chroma_case(const VtClip *base, uint32_t planar_fmt, const Form *form)
{
    VtClip clip;
    char *msg = vt_clip_to_chroma(base, planar_fmt, &clip) ? NULL : "chroma clip";
    msg = msg ? msg : compare_case(&clip, &clip, form);
    vt_clip_close(&clip);
    return msg;
}

static char *test_fixture_wide_chroma(void)
{
    static const Form nv16 = {VMAFX_PIXEL_FORMAT_NV16, 8u, 0u};
    static const Form nv24 = {VMAFX_PIXEL_FORMAT_NV24, 8u, 0u};
    static const Form p210 = {VMAFX_PIXEL_FORMAT_P210, 10u, 6u};
    static const Form p410 = {VMAFX_PIXEL_FORMAT_P410, 10u, 6u};
    static const Form y210 = {VMAFX_PIXEL_FORMAT_Y210, 10u, 6u};
    static const Form y410 = {VMAFX_PIXEL_FORMAT_Y410, 10u, 0u};
    VtClip nv;
    VtClip sp;
    const bool have_nv = vt_clip_open(&nv, &vt_inputs[0]);
    const bool have_sp = vt_clip_open(&sp, &vt_inputs[3]);
    char *msg = NULL;
    if (have_nv && have_sp) {
        msg = chroma_case(&nv, VMAFX_PIXEL_FORMAT_YUV422P, &nv16);
        msg = msg ? msg : chroma_case(&nv, VMAFX_PIXEL_FORMAT_YUV444P, &nv24);
        msg = msg ? msg : chroma_case(&sp, VMAFX_PIXEL_FORMAT_YUV422P, &p210);
        msg = msg ? msg : chroma_case(&sp, VMAFX_PIXEL_FORMAT_YUV444P, &p410);
        msg = msg ? msg : chroma_case(&sp, VMAFX_PIXEL_FORMAT_YUV422P, &y210);
        msg = msg ? msg : chroma_case(&sp, VMAFX_PIXEL_FORMAT_YUV444P, &y410);
    }
    vt_clip_close(&nv);
    vt_clip_close(&sp);
    mu_assert_msg(msg);
    mu_assert("every input present or none", have_nv == have_sp);
    return NULL;
}

/* A clip scaled up to `bpc` bits (its samples shifted left by the difference),
 * for the 12- and 16-bit formats. */
static void clip_scale_up(VtClip *clip, uint32_t planar_fmt, uint32_t bpc)
{
    const unsigned shift = bpc - clip->desc.bpc;
    const VmafxFrameDesc scaled = vt_desc(planar_fmt, bpc, clip->desc.w, clip->desc.h);
    const size_t frame = vt_frame_bytes(&clip->desc);
    for (unsigned i = 0; i < clip->n_frames; i++) {
        vt_shift_up(&scaled, clip->ref + (size_t)i * frame, shift);
        vt_shift_up(&scaled, clip->dist + (size_t)i * frame, shift);
    }
    clip->desc = scaled;
}

/* The formats decoders were measured to emit beyond the 10-bit ones
 * (ADR-2133): YUYV422 and VUYX at 8 bits, Y212 and XV36 at 12 bits, and the
 * MSB-aligned planar 4:4:4 words at 10 and 12 bits. */
static char *scaled_case(const VtClip *base, uint32_t planar_fmt, uint32_t bpc, const Form *form)
{
    VtClip clip;
    if (!vt_clip_to_chroma(base, planar_fmt, &clip)) {
        vt_clip_close(&clip);
        return "chroma clip";
    }
    if (bpc != clip.desc.bpc) {
        clip_scale_up(&clip, planar_fmt, bpc);
    }
    char *const msg = compare_case(&clip, &clip, form);
    vt_clip_close(&clip);
    return msg;
}

static char *test_fixture_other_layouts(void)
{
    static const Form yuy2 = {VMAFX_PIXEL_FORMAT_YUYV422, 8u, 0u};
    static const Form vuyx = {VMAFX_PIXEL_FORMAT_VUYX, 8u, 0u};
    static const Form y212 = {VMAFX_PIXEL_FORMAT_Y212, 12u, 4u};
    static const Form xv36 = {VMAFX_PIXEL_FORMAT_XV36, 12u, 4u};
    static const Form msb10 = {VMAFX_PIXEL_FORMAT_YUV444P_MSB, 10u, 0u};
    static const Form msb12 = {VMAFX_PIXEL_FORMAT_YUV444P_MSB, 12u, 0u};
    VtClip nv;
    VtClip sp;
    const bool have_nv = vt_clip_open(&nv, &vt_inputs[0]);
    const bool have_sp = vt_clip_open(&sp, &vt_inputs[3]);
    char *msg = NULL;
    if (have_nv && have_sp) {
        msg = scaled_case(&nv, VMAFX_PIXEL_FORMAT_YUV422P, 8u, &yuy2);
        msg = msg ? msg : scaled_case(&nv, VMAFX_PIXEL_FORMAT_YUV444P, 8u, &vuyx);
        msg = msg ? msg : scaled_case(&sp, VMAFX_PIXEL_FORMAT_YUV422P, 12u, &y212);
        msg = msg ? msg : scaled_case(&sp, VMAFX_PIXEL_FORMAT_YUV444P, 12u, &xv36);
        msg = msg ? msg : scaled_case(&sp, VMAFX_PIXEL_FORMAT_YUV444P, 10u, &msb10);
        msg = msg ? msg : scaled_case(&sp, VMAFX_PIXEL_FORMAT_YUV444P, 12u, &msb12);
    }
    vt_clip_close(&nv);
    vt_clip_close(&sp);
    mu_assert_msg(msg);
    mu_assert("every input present or none", have_nv == have_sp);
    return NULL;
}

/* P216 and P416: the sparks chroma-widened pair scaled to 16 bits. */
static char *wide_16_case(const VtClip *base, uint32_t planar_fmt, const Form *form)
{
    VtClip clip;
    if (!vt_clip_to_chroma(base, planar_fmt, &clip)) {
        vt_clip_close(&clip);
        return "chroma clip";
    }
    const VmafxFrameDesc d16 = vt_desc(planar_fmt, 16, clip.desc.w, clip.desc.h);
    const size_t frame = vt_frame_bytes(&clip.desc);
    for (unsigned i = 0; i < clip.n_frames; i++) {
        vt_shift_up(&d16, clip.ref + (size_t)i * frame, 6u);
        vt_shift_up(&d16, clip.dist + (size_t)i * frame, 6u);
    }
    clip.desc = d16;
    char *const msg = compare_case(&clip, &clip, form);
    vt_clip_close(&clip);
    return msg;
}

static char *test_fixture_wide_chroma_16(void)
{
    static const Form p216 = {VMAFX_PIXEL_FORMAT_P216, 16u, 0u};
    static const Form p416 = {VMAFX_PIXEL_FORMAT_P416, 16u, 0u};
    VtClip sp;
    if (!vt_clip_open(&sp, &vt_inputs[3])) {
        vt_clip_close(&sp);
        return NULL;
    }
    char *msg = wide_16_case(&sp, VMAFX_PIXEL_FORMAT_YUV422P, &p216);
    msg = msg ? msg : wide_16_case(&sp, VMAFX_PIXEL_FORMAT_YUV444P, &p416);
    vt_clip_close(&sp);
    return msg;
}

/* One synthetic clip of `w` x `h` at `bpc` bits with its chroma widened to
 * `planar_fmt`, imported as `form`. */
static char *odd_case(uint32_t bpc, uint32_t planar_fmt, const Form *form)
{
    VtClip base;
    const bool made = clip_synthetic(&base, 321u, 243u, bpc);
    char *const msg = made ? chroma_case(&base, planar_fmt, form) : "synthetic clip";
    vt_clip_close(&base);
    return msg;
}

/* Odd sizes: the 4:2:2 chroma width rounds up (Y210's last group). */
static char *test_synthetic_wide_odd(void)
{
    static const Form y210 = {VMAFX_PIXEL_FORMAT_Y210, 10u, 6u};
    static const Form nv16 = {VMAFX_PIXEL_FORMAT_NV16, 8u, 0u};
    static const Form y410 = {VMAFX_PIXEL_FORMAT_Y410, 10u, 0u};
    char *msg = odd_case(10u, VMAFX_PIXEL_FORMAT_YUV422P, &y210);
    msg = msg ? msg : odd_case(10u, VMAFX_PIXEL_FORMAT_YUV444P, &y410);
    msg = msg ? msg : odd_case(8u, VMAFX_PIXEL_FORMAT_YUV422P, &nv16);
    return msg;
}

/* Two synthetic frames of `w` x `h` at `bpc`, imported as `form`. */
static char *synthetic_case(uint32_t w, uint32_t h, uint32_t bpc, const Form *form)
{
    VtClip clip;
    const bool made = clip_synthetic(&clip, w, h, bpc);
    char *const msg = made ? compare_case(&clip, &clip, form) : "synthetic clip";
    vt_clip_close(&clip);
    return msg;
}

static char *test_synthetic_4k(void)
{
    static const Form nv12 = {VMAFX_PIXEL_FORMAT_NV12, 8u, 0u};
    static const Form p010 = {VMAFX_PIXEL_FORMAT_P010, 10u, 6u};
    mu_assert_msg(synthetic_case(3840u, 2160u, 8u, &nv12));
    /* Odd size: the chroma planes round up. */
    mu_assert_msg(synthetic_case(3841u, 2161u, 10u, &p010));
    return NULL;
}

/* The counters over every import above. */
static char *test_no_host_copy(void)
{
    (void)fprintf(stderr, "[%lu values, %llu frames imported] ", compared,
                  (unsigned long long)imported);
    mu_assert("imported", imported > 0u);
    mu_assert("every semi-planar import converted on the device",
              vmafx_test_conversions() == imported);
    mu_assert("no host copy of imported pixels", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* ---- One import, many contexts ----------------------------------------------------------- */

/* Two contexts with different models scoring one clip's imports. */
typedef struct Shared {
    VtClip clip;
    VmafxModel *models[2];
    VmafxContext *contexts[2];
    uint8_t *bufs;
    VmafxFence last; /* release fence of the last reference frame */
} Shared;

static bool shared_open(Shared *s)
{
    memset(s, 0, sizeof(*s));
    s->last = (VmafxFence)VMAFX_FENCE_INIT;
    if (!clip_synthetic(&s->clip, 352u, 288u, 8u) ||
        vmafx_model_load(NULL, MODEL, &s->models[0], NULL) != VMAFX_OK ||
        vmafx_model_load(NULL, "vmaf_v0.6.1", &s->models[1], NULL) != VMAFX_OK) {
        return false;
    }
    s->contexts[0] = model_context(s->models[0]);
    s->contexts[1] = model_context(s->models[1]);
    s->bufs = malloc(2u * (size_t)s->clip.n_frames * vt_frame_bytes(&s->clip.desc));
    return s->contexts[0] && s->contexts[1] && s->bufs;
}

static void shared_close(Shared *s)
{
    for (unsigned c = 0; c < 2u; c++) {
        if (s->contexts[c]) {
            (void)vmafx_context_destroy(s->contexts[c], NULL);
        }
        vmafx_model_unref(s->models[c]);
    }
    (void)vmafx_fence_destroy(&s->last, NULL);
    free(s->bufs);
    vt_clip_close(&s->clip);
}

typedef struct Pair {
    VmafxFrame *ref;
    VmafxFrame *dist;
} Pair;

/* Submit one reference of each frame of `pair` to both contexts and drop
 * the import's own. */
static char *submit_both(VmafxContext *const contexts[2], const Pair *pair, unsigned i)
{
    for (unsigned c = 0; c < 2u; c++) {
        mu_assert("submit", vmafx_submit(contexts[c], vmafx_frame_ref(pair->ref),
                                         vmafx_frame_ref(pair->dist), i, NULL) == VMAFX_OK);
    }
    vmafx_frame_unref(pair->ref);
    vmafx_frame_unref(pair->dist);
    return NULL;
}

/* Import every frame once and submit it to both contexts. */
static char *shared_submit(Shared *s)
{
    static const Form nv12 = {VMAFX_PIXEL_FORMAT_NV12, 8u, 0u};
    const size_t frame = vt_frame_bytes(&s->clip.desc);
    const uint64_t attempts = vmafx_test_import_attempts();
    for (unsigned i = 0; i < s->clip.n_frames; i++) {
        uint8_t *const buf = s->bufs + 2u * (size_t)i * frame;
        const Pair pair = {
            import_frame(&s->clip, &nv12, s->clip.ref + (size_t)i * frame, buf),
            import_frame(&s->clip, &nv12, s->clip.dist + (size_t)i * frame, buf + frame)};
        mu_assert("imported", pair.ref && pair.dist);
        mu_assert("previous fence", vmafx_fence_destroy(&s->last, NULL) == VMAFX_OK);
        mu_assert("fence", vmafx_frame_release_fence(pair.ref, VMAFX_FENCE_HOST, &s->last, NULL) ==
                               VMAFX_OK);
        mu_assert_msg(submit_both(s->contexts, &pair, i));
    }
    mu_assert("one import per frame",
              vmafx_test_import_attempts() - attempts == 2u * (uint64_t)s->clip.n_frames);
    return NULL;
}

/* Each context's scores equal a run of its own on host frames. */
static char *shared_compare(Shared *s)
{
    for (unsigned c = 0; c < 2u; c++) {
        mu_assert("flush", vmafx_flush(s->contexts[c], NULL) == VMAFX_OK);
        VmafxContext *const alone = run_host(&s->clip, s->models[c]);
        char *const msg =
            alone ? compare_contexts(alone, s->contexts[c], s->models[c], s->clip.n_frames) :
                    "alone";
        const bool destroyed = !alone || vmafx_context_destroy(alone, NULL) == VMAFX_OK;
        mu_assert_msg(msg);
        mu_assert("destroy alone", destroyed);
    }
    return NULL;
}

/* The last frame is released after the last reader of either context. */
static char *shared_release(Shared *s)
{
    const VmafxStatus first = vmafx_context_destroy(s->contexts[0], NULL);
    s->contexts[0] = NULL;
    mu_assert("first context", first == VMAFX_OK);
    mu_assert("the second still reads the last frame",
              vmafx_fence_wait(&s->last, 0u, NULL) == VMAFX_PENDING);
    const VmafxStatus second = vmafx_context_destroy(s->contexts[1], NULL);
    s->contexts[1] = NULL;
    mu_assert("second context", second == VMAFX_OK);
    mu_assert("released after the last reader", vmafx_fence_wait(&s->last, 0u, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_one_import_two_contexts(void)
{
    Shared s;
    char *msg = shared_open(&s) ? NULL : "setup";
    msg = msg ? msg : shared_submit(&s);
    msg = msg ? msg : shared_compare(&s);
    msg = msg ? msg : shared_release(&s);
    shared_close(&s);
    return msg;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    static const MuTest tests[] = {
        MU_TEST(test_fixture_imports),
        MU_TEST(test_fixture_p016),
        MU_TEST(test_synthetic_4k),
        MU_TEST(test_fixture_wide_chroma),
        MU_TEST(test_fixture_wide_chroma_16),
        MU_TEST(test_fixture_other_layouts),
        MU_TEST(test_synthetic_wide_odd),
        MU_TEST(test_no_host_copy),
        MU_TEST(test_one_import_two_contexts),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
