/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The DEBUG lines of picture conversion (Netflix/vmaf PR 1710): a run whose
 * pictures need no conversion logs one "none needed" line, however many frames
 * it converts, and a conversion context logs its source and target once at
 * creation (zimg builds). A sink at INFO receives neither line. Models that
 * disagree on the conversion target are refused with one ERROR line naming the
 * model.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "conversion_context.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define NONE_NEEDED_PREFIX "picture conversion: none needed"
#define CONVERSION_PREFIX "picture conversion: "
#define CONVERSION_DETAIL "16x16 8bpc limited/bt709/bt709/bt709 -> 16x16 8bpc"

#define MISMATCH_TEXT "has a different conversion target"

typedef struct LogCapture {
    int none_needed;
    int conversion;
    int detail;
    int mismatch;
    bool restored;
} LogCapture;

static void capture_line(enum VmafLogLevel level, const char *message, void *user)
{
    LogCapture *capture = user;
    if (level == VMAF_LOG_LEVEL_ERROR && strstr(message, MISMATCH_TEXT) != NULL)
        capture->mismatch++;
    if (level != VMAF_LOG_LEVEL_DEBUG)
        return;
    if (strncmp(message, NONE_NEEDED_PREFIX, strlen(NONE_NEEDED_PREFIX)) == 0) {
        capture->none_needed++;
        return;
    }
    if (strncmp(message, CONVERSION_PREFIX, strlen(CONVERSION_PREFIX)) == 0)
        capture->conversion++;
    if (strstr(message, CONVERSION_DETAIL) != NULL)
        capture->detail++;
}

/* The thread sink and the counts it writes outlive every frame that installs
 * it: the log keeps the sink pointer in a thread-local until the caller
 * restores the previous sink, so neither may live on a caller's stack. */
static LogCapture capture_counts;
static VmafLogSink capture_sink = {.deliver = capture_line, .user = &capture_counts};

static const VmafLogSink *install_capture(enum VmafLogLevel level)
{
    memset(&capture_counts, 0, sizeof(capture_counts));
    capture_sink.level = level;
    return vmaf_log_swap_thread_sink(&capture_sink);
}

/* Puts the previous sink back, copies the counts out and records whether the
 * capture sink was the one installed and the previous one is current again. */
static void restore_capture(LogCapture *capture, const VmafLogSink *previous)
{
    const VmafLogSink *installed = vmaf_log_swap_thread_sink(previous);
    *capture = capture_counts;
    capture->restored = installed == &capture_sink && vmaf_log_thread_sink() == previous;
}

typedef struct NoConversionRun {
    int first_rc;
    int second_rc;
    LogCapture capture;
} NoConversionRun;

/* Two frames through a state without a conversion target, logged at `level`. */
static NoConversionRun run_no_conversion(enum VmafLogLevel level)
{
    NoConversionRun r;
    memset(&r, 0, sizeof(r));
    VmafConversionState state;
    memset(&state, 0, sizeof(state));
    VmafPicture ref;
    VmafPicture dist;
    memset(&ref, 0, sizeof(ref));
    memset(&dist, 0, sizeof(dist));
    const VmafLogSink *previous = install_capture(level);
    r.first_rc = vmaf_conversion_state_convert(&state, &ref, &dist);
    r.second_rc = vmaf_conversion_state_convert(&state, &ref, &dist);
    restore_capture(&r.capture, previous);
    vmaf_conversion_state_close(&state);
    return r;
}

static char *test_no_conversion_logs_once(void)
{
    const NoConversionRun r = run_no_conversion(VMAF_LOG_LEVEL_DEBUG);
    mu_assert("a run without a conversion target must convert nothing",
              r.first_rc == 0 && r.second_rc == 0);
    mu_assert("two frames without conversion must log exactly one DEBUG line",
              r.capture.none_needed == 1);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

static char *test_no_conversion_silent_at_info(void)
{
    const NoConversionRun r = run_no_conversion(VMAF_LOG_LEVEL_INFO);
    mu_assert("a run without a conversion target must convert nothing",
              r.first_rc == 0 && r.second_rc == 0);
    mu_assert("a sink at INFO must not receive the DEBUG line", r.capture.none_needed == 0);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

typedef struct RegisterRun {
    int first_rc;
    int second_rc;
    LogCapture capture;
} RegisterRun;

/* A model that declares a 4:4:4 8-bit BT.709 target, or none when `enabled` is
 * false; `bpc` tells two targets apart. */
static VmafModel target_model(bool enabled, unsigned bpc)
{
    VmafModel model;
    memset(&model, 0, sizeof(model));
    static char with_name[] = "with_target";
    static char without_name[] = "without_target";
    model.name = enabled ? with_name : without_name;
    model.conversion_target.enabled = enabled;
    model.conversion_target.pix_fmt = VMAF_PIX_FMT_YUV444P;
    model.conversion_target.bpc = bpc;
    model.conversion_target.color.range = VMAF_COLOR_RANGE_LIMITED;
    model.conversion_target.color.primaries = VMAF_COLOR_PRIMARIES_BT709;
    model.conversion_target.color.trc = VMAF_COLOR_TRC_BT709;
    model.conversion_target.color.matrix = VMAF_COLOR_MATRIX_BT709;
    return model;
}

/* Register `first`, then `second`, on one state, logged at ERROR. */
static RegisterRun run_register(const VmafModel *first, const VmafModel *second)
{
    RegisterRun r;
    memset(&r, 0, sizeof(r));
    VmafConversionState state;
    memset(&state, 0, sizeof(state));
    const VmafLogSink *previous = install_capture(VMAF_LOG_LEVEL_ERROR);
    r.first_rc = vmaf_conversion_state_register_model(&state, first);
    r.second_rc = vmaf_conversion_state_register_model(&state, second);
    restore_capture(&r.capture, previous);
    vmaf_conversion_state_close(&state);
    return r;
}

static char *test_register_target_then_none_refused(void)
{
    const VmafModel with = target_model(true, 8);
    const VmafModel without = target_model(false, 8);
    const RegisterRun r = run_register(&with, &without);
    mu_assert("the first model must register", r.first_rc == 0);
    mu_assert("a model without a target after one with must be refused", r.second_rc == -EINVAL);
    mu_assert("the refusal must log one ERROR line", r.capture.mismatch == 1);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

static char *test_register_none_then_target_refused(void)
{
    const VmafModel without = target_model(false, 8);
    const VmafModel with = target_model(true, 8);
    const RegisterRun r = run_register(&without, &with);
    mu_assert("the first model must register", r.first_rc == 0);
    mu_assert("a model with a target after one without must be refused", r.second_rc == -EINVAL);
    mu_assert("the refusal must log one ERROR line", r.capture.mismatch == 1);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

static char *test_register_different_targets_refused(void)
{
    const VmafModel eight = target_model(true, 8);
    const VmafModel ten = target_model(true, 10);
    const RegisterRun r = run_register(&eight, &ten);
    mu_assert("the first model must register", r.first_rc == 0);
    mu_assert("a model with another target must be refused", r.second_rc == -EINVAL);
    mu_assert("the refusal must log one ERROR line", r.capture.mismatch == 1);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

static char *test_register_same_target_accepted(void)
{
    const VmafModel a = target_model(true, 8);
    const VmafModel b = target_model(true, 8);
    const RegisterRun r = run_register(&a, &b);
    mu_assert("two models with one target must both register", r.first_rc == 0 && r.second_rc == 0);
    mu_assert("an accepted model must log no ERROR line", r.capture.mismatch == 0);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

#ifdef HAVE_ZIMG
typedef struct ContextRun {
    int alloc_rc;
    int init_rc;
    int close_rc;
    int unref_rc;
    LogCapture capture;
} ContextRun;

/* One 16x16 8-bit 4:2:0 BT.709 picture to 4:4:4, logged at `level`. */
static ContextRun run_context_init(enum VmafLogLevel level)
{
    ContextRun r;
    memset(&r, 0, sizeof(r));
    const VmafColor color = {
        .range = VMAF_COLOR_RANGE_LIMITED,
        .primaries = VMAF_COLOR_PRIMARIES_BT709,
        .trc = VMAF_COLOR_TRC_BT709,
        .matrix = VMAF_COLOR_MATRIX_BT709,
    };
    const VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = color,
    };
    VmafPicture src;
    r.alloc_rc = vmaf_picture_alloc(&src, VMAF_PIX_FMT_YUV420P, 8, 16, 16);
    if (r.alloc_rc)
        return r;
    VmafPictureConvertContext *ctx = NULL;
    const VmafLogSink *previous = install_capture(level);
    r.init_rc = vmaf_picture_convert_context_init_with_color(&ctx, &src, &color, &target);
    restore_capture(&r.capture, previous);
    r.close_rc = ctx ? vmaf_picture_convert_context_close(ctx) : 0;
    r.unref_rc = vmaf_picture_unref(&src);
    return r;
}

static char *test_context_logs_source_and_target(void)
{
    const ContextRun r = run_context_init(VMAF_LOG_LEVEL_DEBUG);
    mu_assert("vmaf_picture_alloc failed", r.alloc_rc == 0);
    mu_assert("the conversion context could not be created", r.init_rc == 0);
    mu_assert("closing the context or the picture failed", r.close_rc == 0 && r.unref_rc == 0);
    mu_assert("a new context must log exactly one DEBUG line", r.capture.conversion == 1);
    mu_assert("the DEBUG line must name the source and the target", r.capture.detail == 1);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}

static char *test_context_silent_at_info(void)
{
    const ContextRun r = run_context_init(VMAF_LOG_LEVEL_INFO);
    mu_assert("the conversion context could not be created", r.alloc_rc == 0 && r.init_rc == 0);
    mu_assert("a sink at INFO must not receive the DEBUG line", r.capture.conversion == 0);
    mu_assert("the previous log sink must be current again", r.capture.restored);
    return NULL;
}
#endif

char *run_tests(void)
{
    mu_run_test(test_no_conversion_logs_once);
    mu_run_test(test_no_conversion_silent_at_info);
    mu_run_test(test_register_target_then_none_refused);
    mu_run_test(test_register_none_then_target_refused);
    mu_run_test(test_register_different_targets_refused);
    mu_run_test(test_register_same_target_accepted);
#ifdef HAVE_ZIMG
    mu_run_test(test_context_logs_source_and_target);
    mu_run_test(test_context_silent_at_info);
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
