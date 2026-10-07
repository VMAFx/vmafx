/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * What a submitted frame pair must carry (ADR-2094): the colour of a frame
 * (VmafxFrameDesc.color, else the context's default from
 * vmafx_context_set_default_color()) reaches a model's conversion_target
 * (ADR-2093), a change after the first converted pair is VMAFX_E_BUSY, and the
 * `check_sample_range` context option refuses a sample above 2^bpc - 1
 * (ADR-1918).
 *
 * Which colour reached the engine shows in every build: a pair whose colour
 * is fully specified and is not the model's target converts (VMAFX_OK), or is
 * VMAFX_E_NOTSUP without the conversion library (zimg); a pair without a
 * fully specified colour is VMAFX_E_INVALID.
 *
 * Failing first: on the branch without the submit's colour hand-over every
 * frame-colour and default-colour case scored VMAFX_E_INVALID, and
 * vmafx_context_set_option() refused `check_sample_range` (VMAFX_E_NOTFOUND).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conversion_target_model.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, BPC = 10, POOL = 4 };

#define MODEL_FILE "test_vmafx_frame_input_model.json"

/* A pair that needs a conversion: converted, or no conversion library. */
#ifdef HAVE_ZIMG
#define CONVERTED VMAFX_OK
#else
#define CONVERTED VMAFX_E_NOTSUP
#endif

static const VmafxColor pq_ncl = {VMAFX_COLOR_RANGE_LIMITED, VMAFX_COLOR_PRIMARIES_BT2020,
                                  VMAFX_COLOR_TRC_SMPTE2084, VMAFX_COLOR_MATRIX_BT2020_NCL};
static const VmafxColor sdr = {VMAFX_COLOR_RANGE_LIMITED, VMAFX_COLOR_PRIMARIES_BT709,
                               VMAFX_COLOR_TRC_BT709, VMAFX_COLOR_MATRIX_BT709};
static const VmafxColor range_only = {VMAFX_COLOR_RANGE_LIMITED, 0, 0, 0};

static VmafxFrameDesc colour_desc(const VmafxColor *color)
{
    VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, BPC, W, H);
    if (color) {
        d.color = *color;
    }
    return d;
}

/* A host frame of `d` with deterministic in-range content. */
static VmafxFrame *desc_frame(const VmafxFrameDesc *d, unsigned seed)
{
    uint8_t *const data = malloc(vt_frame_bytes(d));
    if (!data) {
        return NULL;
    }
    vt_fill(d, data, seed);
    VmafxFrame *const frame = vt_copy_frame(d, data);
    free(data);
    return frame;
}

static VmafxFrame *colour_frame(const VmafxColor *color, unsigned seed)
{
    const VmafxFrameDesc d = colour_desc(color);
    return desc_frame(&d, seed);
}

/* A context scoring the conversion-target model (it holds the model). */
static VmafxContext *target_context(void)
{
    VmafxModel *model = NULL;
    const int written = conversion_target_model_write(MODEL_FILE);
    const VmafxStatus loaded =
        written == 0 ? vmafx_model_load_file(NULL, MODEL_FILE, &model, NULL) : VMAFX_E_IO;
    const bool removed = written == 0 && remove(MODEL_FILE) == 0;
    VmafxContext *context = NULL;
    if (loaded == VMAFX_OK && removed && vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
        vmafx_context_use_model(context, model, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        context = NULL;
    }
    vmafx_model_unref(model);
    return context;
}

/* Submit a pair of `ref` / `dist`; a refusal of the engine names the index. */
static VmafxStatus submit_frames(VmafxContext *context, VmafxFrame *ref, VmafxFrame *dist,
                                 uint64_t index, bool *named)
{
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_submit(context, ref, dist, index, &error);
    *named = status == VMAFX_OK || vt_failed(&error, status, "index", VMAFX_SUBJECT_FRAME);
    vmafx_error_free(error);
    return status;
}

/* Submit a pair carrying `ref` / `dist` (NULL: none). */
static VmafxStatus submit_colours(VmafxContext *context, const VmafxColor *ref,
                                  const VmafxColor *dist, uint64_t index)
{
    VmafxFrame *const r = colour_frame(ref, (unsigned)index);
    VmafxFrame *const d = colour_frame(dist, (unsigned)index + 7u);
    if (!r || !d) {
        vmafx_frame_unref(r);
        vmafx_frame_unref(d);
        return VMAFX_E_NOMEM;
    }
    bool named = false;
    const VmafxStatus status = submit_frames(context, r, d, index, &named);
    return named ? status : VMAFX_E_INTERNAL;
}

/* ---- Frame colour ------------------------------------------------------------ */

static char *test_frame_colour_reaches_the_conversion(void)
{
    VmafxContext *context = target_context();
    mu_assert("context", context != NULL);
    mu_assert("no colour, no default", submit_colours(context, NULL, NULL, 0) == VMAFX_E_INVALID);
    mu_assert("part of a colour",
              submit_colours(context, &range_only, &pq_ncl, 1) == VMAFX_E_INVALID);
    mu_assert("both frames carry one", submit_colours(context, &pq_ncl, &pq_ncl, 2) == CONVERTED);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_default_colour_for_frames_without_one(void)
{
    VmafxContext *context = target_context();
    VmafxError *error = NULL;
    mu_assert("NULL context",
              vmafx_context_set_default_color(NULL, &pq_ncl, &pq_ncl, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "context", VMAFX_SUBJECT_PARAMETER));
    mu_assert("set", vmafx_context_set_default_color(context, &pq_ncl, &pq_ncl, NULL) == VMAFX_OK);
    mu_assert("unset", vmafx_context_set_default_color(context, NULL, NULL, NULL) == VMAFX_OK);
    mu_assert("unset again", submit_colours(context, NULL, NULL, 0) == VMAFX_E_INVALID);
    mu_assert("set", vmafx_context_set_default_color(context, &pq_ncl, &pq_ncl, NULL) == VMAFX_OK);
    mu_assert("the default", submit_colours(context, NULL, NULL, 1) == CONVERTED);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_frame_colour_wins_over_the_default(void)
{
    VmafxContext *context = target_context();
    mu_assert("context", context != NULL);
    mu_assert("a default that would be refused",
              vmafx_context_set_default_color(context, &range_only, &pq_ncl, NULL) == VMAFX_OK);
    mu_assert("each input on its own: the reference's colour, the distorted default",
              submit_colours(context, &pq_ncl, NULL, 0) == CONVERTED);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* A caller compiled before VmafxFrameDesc.color: its frames carry none. */
static char *test_older_desc_carries_no_colour(void)
{
    VmafxFrameDesc d = colour_desc(&pq_ncl);
    d.struct_size = (uint32_t)offsetof(VmafxFrameDesc, color);
    VmafxFrame *const ref = desc_frame(&d, 0);
    VmafxFrame *const dist = desc_frame(&d, 1);
    mu_assert("older struct accepted", ref != NULL && dist != NULL);
    VmafxContext *context = target_context();
    bool named = false;
    mu_assert("colour past struct_size unread",
              submit_frames(context, ref, dist, 0, &named) == VMAFX_E_INVALID && named);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Frames from every maker keep the colour ------------------------------------ */

/* Zero the samples of a frame whose memory the library allocated. */
static VmafxStatus clear_frame(const VmafxFrame *frame)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    const VmafxStatus status = vmafx_frame_planes(frame, &planes, NULL);
    for (uint32_t p = 0; status == VMAFX_OK && p < planes.n_planes && p < 3u; p++) {
        memset(planes.data[p], 0, (size_t)planes.stride[p] * planes.h[p]);
    }
    return status;
}

static VmafxStatus submit_pool_frames(VmafxContext *context, VmafxFramePool *pool)
{
    VmafxFrame *ref = NULL;
    VmafxFrame *dist = NULL;
    VmafxStatus status = vmafx_frame_pool_acquire(pool, &ref, NULL);
    if (status == VMAFX_OK) {
        status = vmafx_frame_pool_acquire(pool, &dist, NULL);
    }
    if (status == VMAFX_OK) {
        status = clear_frame(ref) == VMAFX_OK ? clear_frame(dist) : VMAFX_E_INTERNAL;
    }
    if (status != VMAFX_OK) {
        vmafx_frame_unref(ref);
        vmafx_frame_unref(dist);
        return status;
    }
    bool named = false;
    status = submit_frames(context, ref, dist, 0, &named);
    return named ? status : VMAFX_E_INTERNAL;
}

static char *test_pool_and_wrapped_frames_keep_the_colour(void)
{
    const VmafxFrameDesc d = colour_desc(&pq_ncl);
    VmafxFramePool *pool = NULL;
    mu_assert("pool", vmafx_frame_pool_create(NULL, &d, POOL, &pool, NULL) == VMAFX_OK);
    VmafxContext *context = target_context();
    mu_assert("pool frames", submit_pool_frames(context, pool) == CONVERTED);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    vmafx_frame_pool_destroy(pool);

    uint8_t *const data = calloc(1, vt_frame_bytes(&d));
    mu_assert("data", data != NULL);
    context = target_context();
    bool named = false;
    const VmafxStatus status = submit_frames(context, vt_wrap_frame(&d, data, NULL),
                                             vt_wrap_frame(&d, data, NULL), 0, &named);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    free(data);
    mu_assert("wrapped frames", status == CONVERTED && named);
    return NULL;
}

static char *test_preallocated_frames_keep_the_colour(void)
{
    VmafxContext *context = target_context();
    const VmafxFrameDesc d = colour_desc(&pq_ncl);
    mu_assert("preallocate", vmafx_context_preallocate(context, &d, POOL, NULL) == VMAFX_OK);
    VmafxFrame *ref = NULL;
    VmafxFrame *dist = NULL;
    mu_assert("acquire", vmafx_context_acquire_frame(context, &ref, NULL) == VMAFX_OK &&
                             vmafx_context_acquire_frame(context, &dist, NULL) == VMAFX_OK);
    mu_assert("clear", clear_frame(ref) == VMAFX_OK && clear_frame(dist) == VMAFX_OK);
    bool named = false;
    mu_assert("preallocated frames", submit_frames(context, ref, dist, 0, &named) == CONVERTED);
    mu_assert("named", named);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- A change after the first converted pair ------------------------------------- */

#ifdef HAVE_ZIMG
static char *test_busy_after_the_first_converted_pair(void)
{
    VmafxContext *context = target_context();
    mu_assert("first pair converts", submit_colours(context, &pq_ncl, &pq_ncl, 0) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("default after it",
              vmafx_context_set_default_color(context, &pq_ncl, &pq_ncl, &error) == VMAFX_E_BUSY &&
                  vt_failed(&error, VMAFX_E_BUSY, "context", VMAFX_SUBJECT_CONTEXT));
    mu_assert("another colour", submit_colours(context, &sdr, &sdr, 1) == VMAFX_E_BUSY);
    mu_assert("no colour", submit_colours(context, NULL, NULL, 2) == VMAFX_E_BUSY);
    mu_assert("the same colour", submit_colours(context, &pq_ncl, &pq_ncl, 3) == VMAFX_OK);
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}
#else
static char *test_nothing_converted_keeps_the_default_open(void)
{
    VmafxContext *context = target_context();
    mu_assert("refused pair", submit_colours(context, &pq_ncl, &pq_ncl, 0) == VMAFX_E_NOTSUP);
    mu_assert("default", vmafx_context_set_default_color(context, &sdr, &sdr, NULL) == VMAFX_OK);
    mu_assert("another colour", submit_colours(context, &sdr, &sdr, 1) == VMAFX_E_NOTSUP);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}
#endif

/* ---- The check_sample_range option (ADR-1918) -------------------------------------- */

/* Submit a 10-bit pair whose distorted frame holds `sample` at one position. */
static VmafxStatus submit_sample(VmafxContext *context, uint16_t sample, uint64_t index)
{
    const VmafxFrameDesc d = colour_desc(NULL);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    if (!data) {
        return VMAFX_E_NOMEM;
    }
    vt_fill(&d, data, (unsigned)index);
    VmafxFrame *const ref = vt_copy_frame(&d, data);
    memcpy(data + (size_t)2 * ((size_t)W * 3u + 5u), &sample, sizeof(sample));
    VmafxFrame *const dist = vt_copy_frame(&d, data);
    free(data);
    bool named = false;
    const VmafxStatus status = submit_frames(context, ref, dist, index, &named);
    return named ? status : VMAFX_E_INTERNAL;
}

static char *test_check_sample_range_option(void)
{
    VmafxContext *context = vt_psnr_context();
    mu_assert("off by default", context && submit_sample(context, 1500, 0) == VMAFX_OK);
    mu_assert("on", vmafx_context_set_option(context, "check_sample_range", "1", NULL) == VMAFX_OK);
    mu_assert("in range", submit_sample(context, 1023, 1) == VMAFX_OK);
    mu_assert("above 2^bpc - 1", submit_sample(context, 1500, 2) == VMAFX_E_INVALID);
    mu_assert("off",
              vmafx_context_set_option(context, "check_sample_range", "false", NULL) == VMAFX_OK);
    mu_assert("unchecked again", submit_sample(context, 1500, 3) == VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_check_sample_range_refuses_other_values(void)
{
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("a value the option refuses",
              vmafx_context_set_option(context, "check_sample_range", "maybe", &error) ==
                      VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "check_sample_range", VMAFX_SUBJECT_OPTION));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_frame_colour_reaches_the_conversion),
        MU_TEST(test_default_colour_for_frames_without_one),
        MU_TEST(test_frame_colour_wins_over_the_default),
        MU_TEST(test_older_desc_carries_no_colour),
        MU_TEST(test_pool_and_wrapped_frames_keep_the_colour),
        MU_TEST(test_preallocated_frames_keep_the_colour),
#ifdef HAVE_ZIMG
        MU_TEST(test_busy_after_the_first_converted_pair),
#else
        MU_TEST(test_nothing_converted_keeps_the_default_open),
#endif
        MU_TEST(test_check_sample_range_option),
        MU_TEST(test_check_sample_range_refuses_other_values),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
