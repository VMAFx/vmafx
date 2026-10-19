/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Deterministic device-free unit tests for the VPL decode retry ceiling
 * and status-sequence contract (ADR-1287 / ADR-1900).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "vmaf_vpl_core.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define MAX_FAKE_SEQUENCE 64U
#define MAX_PUBLISHED_FRAMES 16U

typedef struct {
    mfxStatus status_sequence[MAX_FAKE_SEQUENCE];
    unsigned sequence_len;
    unsigned sequence_idx;
    mfxStatus infinite_status;
    int use_infinite;

    size_t bitstream_len;
    size_t buffer_size;
    int eof;

    unsigned current_frame_id;
    unsigned attempts_called;
    unsigned backoff_called;
    unsigned total_backoff_us;

    int logged_error_code;
    unsigned logged_exhaustion_attempts;

    unsigned published_frame_ids[MAX_PUBLISHED_FRAMES];
    unsigned published_count;
} FakeVplSession;

static void fake_init(FakeVplSession *f)
{
    memset(f, 0, sizeof(*f));
    f->buffer_size = (size_t)2 * 1024 * 1024;
    f->bitstream_len = 1024U;
}

static size_t fake_get_bitstream_length(void *ctx)
{
    const FakeVplSession *f = (const FakeVplSession *)ctx;
    return f->bitstream_len;
}

static size_t fake_get_buffer_size(void *ctx)
{
    const FakeVplSession *f = (const FakeVplSession *)ctx;
    return f->buffer_size;
}

static int fake_is_eof(void *ctx)
{
    const FakeVplSession *f = (const FakeVplSession *)ctx;
    return f->eof;
}

static int fake_refill_bitstream(void *ctx)
{
    FakeVplSession *f = (FakeVplSession *)ctx;
    f->bitstream_len = f->buffer_size;
    return 0;
}

static mfxStatus fake_decode_async(void *ctx, int passing_null, mfxFrameSurface1 **out_surf,
                                   mfxSyncPoint *out_sync)
{
    FakeVplSession *f = (FakeVplSession *)ctx;
    f->attempts_called++;
    *out_surf = NULL;
    *out_sync = NULL;

    mfxStatus sts = MFX_ERR_NONE;
    if (f->sequence_idx < f->sequence_len) {
        sts = f->status_sequence[f->sequence_idx];
        f->sequence_idx++;
    } else if (f->use_infinite) {
        sts = f->infinite_status;
    } else {
        sts = passing_null ? MFX_ERR_MORE_DATA : MFX_ERR_NONE;
    }

    if ((sts == MFX_ERR_NONE || sts > 0) && sts != MFX_WRN_DEVICE_BUSY &&
        sts != MFX_WRN_ALLOC_TIMEOUT_EXPIRED) {
        /* Produce dummy sync point and surface handle */
        static int dummy_sync_marker = 1;
        static mfxFrameSurface1 dummy_surface;
        *out_sync = (mfxSyncPoint)&dummy_sync_marker;
        *out_surf = &dummy_surface;
    }

    return sts;
}

static int fake_publish_surface(void *ctx, mfxSyncPoint sync, mfxFrameSurface1 *out_surf)
{
    FakeVplSession *f = (FakeVplSession *)ctx;
    (void)sync;
    (void)out_surf;
    if (f->published_count < MAX_PUBLISHED_FRAMES) {
        f->published_frame_ids[f->published_count] = f->current_frame_id;
        f->published_count++;
    }
    f->current_frame_id++;
    return 0;
}

static void fake_backoff(void *ctx, unsigned usec)
{
    FakeVplSession *f = (FakeVplSession *)ctx;
    f->backoff_called++;
    f->total_backoff_us += usec;
}

static void fake_log_error(void *ctx, const char *msg, int code)
{
    FakeVplSession *f = (FakeVplSession *)ctx;
    (void)msg;
    f->logged_error_code = code;
}

static void fake_log_exhaustion(void *ctx, unsigned attempts)
{
    FakeVplSession *f = (FakeVplSession *)ctx;
    f->logged_exhaustion_attempts = attempts;
}

static const VplDecodeDriver g_fake_driver = {
    .get_bitstream_length = fake_get_bitstream_length,
    .get_buffer_size = fake_get_buffer_size,
    .is_eof = fake_is_eof,
    .refill_bitstream = fake_refill_bitstream,
    .decode_async = fake_decode_async,
    .publish_surface = fake_publish_surface,
    .backoff = fake_backoff,
    .log_error = fake_log_error,
    .log_exhaustion = fake_log_exhaustion,
};

static char *test_finite_device_busy_retry_succeeds(void)
{
    FakeVplSession f;
    fake_init(&f);

    /* 10 busy attempts, then success */
    f.sequence_len = 11;
    for (unsigned i = 0; i < 10; i++) {
        f.status_sequence[i] = MFX_WRN_DEVICE_BUSY;
    }
    f.status_sequence[10] = MFX_ERR_NONE;

    const int ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("finite busy sequence should succeed with 0", ret == 0);
    mu_assert("attempt count should be 11", f.attempts_called == 11);
    mu_assert("backoff count should be 10", f.backoff_called == 10);
    mu_assert("total backoff should be 10000 us", f.total_backoff_us == 10000U);
    mu_assert("one frame should be published", f.published_count == 1);
    mu_assert("frame id should be 0", f.published_frame_ids[0] == 0);
    return NULL;
}

static char *test_too_low_ceiling_fails(void)
{
    FakeVplSession f_low;
    fake_init(&f_low);

    /* 25 busy attempts, then success */
    f_low.sequence_len = 26;
    for (unsigned i = 0; i < 25; i++) {
        f_low.status_sequence[i] = MFX_WRN_DEVICE_BUSY;
    }
    f_low.status_sequence[25] = MFX_ERR_NONE;

    /* A too-low ceiling (10 attempts) must fail */
    const int ret_low = vpl_decode_frame_loop(&f_low, &g_fake_driver, 10U, VPL_DECODE_RETRY_US);
    mu_assert("too-low ceiling must fail with -1", ret_low == -1);
    mu_assert("attempt count should match too-low ceiling", f_low.attempts_called == 10);
    mu_assert("exhaustion attempt count logged", f_low.logged_exhaustion_attempts == 10);
    mu_assert("no frame should be published", f_low.published_count == 0);
    return NULL;
}

static char *test_sufficient_ceiling_succeeds(void)
{
    /* A sufficient ceiling (60000 attempts) must succeed on the same pattern */
    FakeVplSession f_high;
    fake_init(&f_high);
    f_high.sequence_len = 26;
    for (unsigned i = 0; i < 25; i++) {
        f_high.status_sequence[i] = MFX_WRN_DEVICE_BUSY;
    }
    f_high.status_sequence[25] = MFX_ERR_NONE;

    const int ret_high = vpl_decode_frame_loop(&f_high, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS,
                                               VPL_DECODE_RETRY_US);
    mu_assert("sufficient ceiling must succeed with 0", ret_high == 0);
    mu_assert("attempt count should be 26", f_high.attempts_called == 26);
    mu_assert("one frame published", f_high.published_count == 1);
    return NULL;
}

static char *test_true_no_progress_loop_terminates_at_60000(void)
{
    FakeVplSession f;
    fake_init(&f);

    /* Perpetual device busy */
    f.use_infinite = 1;
    f.infinite_status = MFX_WRN_DEVICE_BUSY;

    const int ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("true no-progress loop must terminate with -1", ret == -1);
    mu_assert("attempt count must exactly equal 60000",
              f.attempts_called == VPL_DECODE_MAX_ATTEMPTS);
    mu_assert("backoff count must exactly equal 60000",
              f.backoff_called == VPL_DECODE_MAX_ATTEMPTS);
    mu_assert("exhaustion logged at 60000",
              f.logged_exhaustion_attempts == VPL_DECODE_MAX_ATTEMPTS);
    mu_assert("no frame published on wedged device", f.published_count == 0);
    return NULL;
}

static char *test_decoded_frame_ordering_preserved(void)
{
    FakeVplSession f;
    fake_init(&f);

    /* Decode 5 frames in sequence across varying retry profiles */
    const unsigned retries_per_frame[5] = {3U, 0U, 7U, 1U, 4U};

    for (unsigned frame = 0; frame < 5; frame++) {
        f.sequence_len = retries_per_frame[frame] + 1U;
        f.sequence_idx = 0;
        for (unsigned i = 0; i < retries_per_frame[frame]; i++) {
            f.status_sequence[i] = MFX_WRN_DEVICE_BUSY;
        }
        f.status_sequence[retries_per_frame[frame]] = MFX_ERR_NONE;

        const int ret =
            vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
        mu_assert("frame decode must return 0", ret == 0);
    }

    mu_assert("must publish exactly 5 frames", f.published_count == 5);
    for (unsigned i = 0; i < 5; i++) {
        mu_assert("frame index must match exact sequential order", f.published_frame_ids[i] == i);
    }

    /* Drain at EOF returns 1 */
    f.sequence_len = 1;
    f.sequence_idx = 0;
    f.bitstream_len = 0;
    f.eof = 1;
    f.status_sequence[0] = MFX_ERR_MORE_DATA;

    const int eof_ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("drain call at EOF must return 1", eof_ret == 1);
    return NULL;
}

static char *test_warning_with_sync_publishes_frame(void)
{
    FakeVplSession f;
    fake_init(&f);

    /* MFX_WRN_VIDEO_PARAM_CHANGED accompanied by a frame */
    f.sequence_len = 1;
    f.status_sequence[0] = MFX_WRN_VIDEO_PARAM_CHANGED;

    const int ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("warning with valid sync point must publish frame and return 0", ret == 0);
    mu_assert("attempt count must be 1 (frame delivered immediately on warning)",
              f.attempts_called == 1);
    mu_assert("published count must be 1", f.published_count == 1);
    return NULL;
}

static char *test_transient_retryable_statuses(void)
{
    FakeVplSession f;
    fake_init(&f);

    /* MORE_SURFACE, ALLOC_TIMEOUT_EXPIRED, DEVICE_BUSY, then NONE */
    f.sequence_len = 4;
    f.status_sequence[0] = MFX_ERR_MORE_SURFACE;
    f.status_sequence[1] = MFX_WRN_ALLOC_TIMEOUT_EXPIRED;
    f.status_sequence[2] = MFX_WRN_DEVICE_BUSY;
    f.status_sequence[3] = MFX_ERR_NONE;

    const int ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("transient retry sequence must succeed with 0", ret == 0);
    mu_assert("attempt count must be 4", f.attempts_called == 4);
    mu_assert("backoff count must be 3", f.backoff_called == 3);
    mu_assert("published count must be 1", f.published_count == 1);
    return NULL;
}

static char *test_hard_error_fails_immediately(void)
{
    FakeVplSession f;
    fake_init(&f);

    /* Fatal hardware loss */
    f.sequence_len = 1;
    f.status_sequence[0] = MFX_ERR_DEVICE_LOST;

    const int ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("hard error must return -1 immediately", ret == -1);
    mu_assert("attempt count must be 1 (no spin to ceiling)", f.attempts_called == 1);
    mu_assert("logged error code matches status", f.logged_error_code == (int)MFX_ERR_DEVICE_LOST);
    return NULL;
}

static char *test_exhaustion_exact_diagnostic_message(void)
{
    FakeVplSession f;
    fake_init(&f);

    f.use_infinite = 1;
    f.infinite_status = MFX_WRN_DEVICE_BUSY;

    const int ret =
        vpl_decode_frame_loop(&f, &g_fake_driver, VPL_DECODE_MAX_ATTEMPTS, VPL_DECODE_RETRY_US);
    mu_assert("deliberately forced run must return -1", ret == -1);
    mu_assert("logged exhaustion attempts must match 60000",
              f.logged_exhaustion_attempts == 60000U);

    char expected_msg[64];
    const int n = snprintf(expected_msg, sizeof(expected_msg),
                           "DecodeFrameAsync yielded no frame after %u attempts",
                           f.logged_exhaustion_attempts);
    mu_assert("format matches documented diagnostic string",
              n > 0 && strcmp(expected_msg,
                              "DecodeFrameAsync yielded no frame after 60000 attempts") == 0);
    return NULL;
}

static char *run_ceiling_tests(void)
{
    mu_run_test(test_finite_device_busy_retry_succeeds);
    mu_run_test(test_too_low_ceiling_fails);
    mu_run_test(test_sufficient_ceiling_succeeds);
    mu_run_test(test_true_no_progress_loop_terminates_at_60000);
    return NULL;
}

static char *run_status_tests(void)
{
    mu_run_test(test_decoded_frame_ordering_preserved);
    mu_run_test(test_warning_with_sync_publishes_frame);
    mu_run_test(test_transient_retryable_statuses);
    mu_run_test(test_hard_error_fails_immediately);
    mu_run_test(test_exhaustion_exact_diagnostic_message);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(run_ceiling_tests);
    mu_run_test(run_status_tests);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
