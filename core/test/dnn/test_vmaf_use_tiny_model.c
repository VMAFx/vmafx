/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Public-surface tests for vmaf_use_tiny_model() — the ctx-attach path
 *  that was identified as having zero C-unit-test coverage in the 2026-05-16
 *  test-coverage audit (§2, "vmaf_use_tiny_model").
 *
 *  Strategy: link against libvmaf (not compiled-in dnn/ sources) so the test
 *  exercises the stable ABI contract the way downstream consumers do.
 *
 *  When built with -Denable_dnn=false:
 *    - vmaf_dnn_available() == 0
 *    - vmaf_use_tiny_model() must return -ENOSYS (ADR-0374 stub contract)
 *    - null-arg tests are skipped to avoid assert-abort on disabled builds
 *
 *  When built with -Denable_dnn=true:
 *    - null ctx  → -EINVAL
 *    - null path → -EINVAL
 *    - non-existent path → -ENOENT
 *    - smoke_v0.onnx happy path → 0 (session attached, ctx closed cleanly)
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "test.h"

#include "libvmaf/dnn.h"
#include "libvmaf/libvmaf.h"

/* Path is relative to workdir = project root (set in meson.build). */
#define SMOKE_FP32_MODEL "model/tiny/smoke_v0.onnx"
#define SMOKE_MULTI_OUTPUT_MODEL "model/tiny/smoke_multi_output_v0.onnx"
/* ADR-0524: minimal rank-4 NCHW ONNX with `dim_param='batch'` on dim 0.
 * Generated once and committed under model/tiny/; see
 * docs/adr/0524-tiny-model-loader-symbolic-batch-dim.md. The loader
 * folds symbolic batch to 1; attach must succeed even though
 * ORT reports `in_shape[0] == -1`. */
#define SMOKE_SYMBATCH_MODEL "model/tiny/smoke_v0_symbolic_batch.onnx"
/* Rank-5 temporal model fixture. It opens in ORT, then vmaf_use_tiny_model()
 * rejects it at the attach-time shape gate because the public tiny-model
 * frame path supports rank-2 feature vectors and rank-4 NCHW images only. */
#define RANK5_MODEL "model/tiny/transnet_v2.onnx"
#define TINY_V1_MODEL "model/tiny/vmaf_tiny_v1.onnx"

#ifndef _WIN32
static const unsigned char kAllowedOnnx[] = {0x3A, 0x08, 0x0A, 0x06, 0x22,
                                             0x04, 'C',  'o',  'n',  'v'};

static int write_file_600(const char *path, const unsigned char *data, size_t len)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    const ssize_t w = write(fd, data, len);
    const int rc = close(fd);
    if (w != (ssize_t)len || rc != 0)
        return -1;
    return 0;
}

static int copy_file_600(const char *src, const char *dst)
{
    const int in_fd = open(src, O_RDONLY);
    if (in_fd < 0)
        return -1;
    const int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        (void)close(in_fd);
        return -1;
    }

    unsigned char buf[4096];
    int rc = 0;
    for (;;) {
        const ssize_t n = read(in_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            rc = -1;
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
            if (w <= 0) {
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
    }

    if (close(out_fd) != 0)
        rc = -1;
    if (close(in_fd) != 0)
        rc = -1;
    return rc;
}
#endif

/* Helper: allocate a minimal VmafContext (no features, no threads). */
static VmafContext *alloc_ctx(void)
{
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 1,
        .n_subsample = 1,
        .cpumask = 0,
        .gpumask = 0,
    };
    VmafContext *ctx = NULL;
    int rc = vmaf_init(&ctx, cfg);
    if (rc < 0)
        return NULL;
    return ctx;
}

static void fill_luma(VmafPicture *pic, uint8_t value)
{
    for (unsigned y = 0; y < pic->h[0]; ++y) {
        uint8_t *row = (uint8_t *)pic->data[0] + (ptrdiff_t)y * pic->stride[0];
        memset(row, value, pic->w[0]);
    }
}

/* --- stub contract when DNN is disabled ----------------------------------- */

static char *test_returns_enosys_when_disabled(void)
{
    if (vmaf_dnn_available())
        return NULL; /* real ORT — skip this test */

    int rc = vmaf_use_tiny_model(NULL, SMOKE_FP32_MODEL, NULL);
    mu_assert("disabled build: must return -ENOSYS regardless of args", rc == -ENOSYS);
    return NULL;
}

/* --- null-arg rejection (enabled build only) ------------------------------ */

static char *test_rejects_null_ctx(void)
{
    if (!vmaf_dnn_available())
        return NULL; /* stub build — skip */

    int rc = vmaf_use_tiny_model(NULL, SMOKE_FP32_MODEL, NULL);
    mu_assert("null ctx must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_rejects_null_path(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);

    int rc = vmaf_use_tiny_model(ctx, NULL, NULL);
    mu_assert("null path must return -EINVAL", rc == -EINVAL);

    (void)vmaf_close(ctx);
    return NULL;
}

/* --- non-existent path (enabled build only) ------------------------------- */

static char *test_rejects_nonexistent_path(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);

    int rc = vmaf_use_tiny_model(ctx, "/nonexistent/__no_such_file__.onnx", NULL);
    mu_assert("non-existent path must return < 0", rc < 0);

    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_codec_context_and_resize_reject_bad_args(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    int rc = vmaf_dnn_set_codec_context(NULL, "libx264", "medium", 23);
    mu_assert("codec context NULL ctx rejected", rc == -EINVAL);
    rc = vmaf_dnn_set_resize_mode(NULL, VMAF_DNN_RESIZE_BILINEAR);
    mu_assert("resize mode NULL ctx rejected", rc == -EINVAL);

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);
    rc = vmaf_dnn_set_resize_mode(ctx, (VmafDnnResizeMode)99);
    mu_assert("invalid resize mode rejected", rc == -EINVAL);
    rc = vmaf_dnn_set_codec_context(ctx, "libx264", "medium", 23);
    mu_assert("codec context without attached codec-aware model rejected", rc < 0);
    rc = vmaf_dnn_set_resize_mode(ctx, VMAF_DNN_RESIZE_BILINEAR);
    mu_assert("resize mode accepted on live ctx", rc == 0);
    (void)vmaf_close(ctx);
    return NULL;
}

#ifndef _WIN32
static char *test_rejects_oversized_sidecar_before_ort_open(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    if (access(SMOKE_FP32_MODEL, R_OK) != 0)
        return NULL;
    char tmpl[] = "/tmp/vmaf-use-tiny-sidecar-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    (void)close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    mu_assert("copy smoke onnx failed", copy_file_600(SMOKE_FP32_MODEL, onnx) == 0);

    unsigned char chunk[4096];
    memset(chunk, ' ', sizeof(chunk));
    fd = open(sidecar, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    mu_assert("open sidecar failed", fd >= 0);
    for (size_t i = 0; i < 257u; ++i) {
        mu_assert("write sidecar chunk failed",
                  write(fd, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk));
    }
    (void)close(fd);

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);
    int rc = vmaf_use_tiny_model(ctx, onnx, NULL);
    mu_assert("oversized sidecar rejected before ORT open", rc == -EFBIG);
    (void)vmaf_close(ctx);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_valid_scanner_invalid_ort_model_closes_session(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char tmpl[] = "/tmp/vmaf-use-tiny-invalid-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    (void)close(fd);

    char onnx[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    mu_assert("write onnx failed", write_file_600(onnx, kAllowedOnnx, sizeof(kAllowedOnnx)) == 0);

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);
    int rc = vmaf_use_tiny_model(ctx, onnx, NULL);
    mu_assert("protobuf-shaped but invalid ORT model rejected", rc < 0);
    (void)vmaf_close(ctx);

    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_invalid_ort_model_frees_loaded_sidecar(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char tmpl[] = "/tmp/vmaf-use-tiny-sidecar-cleanup-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    (void)close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    mu_assert("write onnx failed", write_file_600(onnx, kAllowedOnnx, sizeof(kAllowedOnnx)) == 0);
    static const unsigned char json[] = "{\"kind\":\"nr\",\"name\":\"cleanup_probe\"}\n";
    mu_assert("write sidecar failed", write_file_600(sidecar, json, sizeof(json) - 1u) == 0);

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);
    int rc = vmaf_use_tiny_model(ctx, onnx, NULL);
    mu_assert("invalid ORT model with sidecar rejected", rc < 0);
    (void)vmaf_close(ctx);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}
#endif

/* --- happy path (enabled build + smoke fixture present) ------------------- */

static char *test_happy_path_smoke_model(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* smoke_v0.onnx may not be present in every CI leg that runs the dnn
     * suite (e.g. MSVC build-only, cross builds without model fixtures). */
#ifndef _WIN32
    if (access(SMOKE_FP32_MODEL, R_OK) != 0)
        return NULL;
#endif

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);

    int rc = vmaf_use_tiny_model(ctx, SMOKE_FP32_MODEL, NULL);
    mu_assert("smoke model attach must return 0", rc == 0);

    /* vmaf_close tears down the DNN session via vmaf_ctx_dnn_attach's
     * ownership transfer — this exercises the teardown path as well. */
    rc = vmaf_close(ctx);
    mu_assert("vmaf_close after tiny model must return 0", rc == 0);
    return NULL;
}

static char *test_attached_multi_output_model_records_named_scores(void)
{
    if (!vmaf_dnn_available())
        return NULL;

#ifndef _WIN32
    if (access(SMOKE_MULTI_OUTPUT_MODEL, R_OK) != 0)
        return NULL;
#endif

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);

    int rc = vmaf_use_tiny_model(ctx, SMOKE_MULTI_OUTPUT_MODEL, NULL);
    mu_assert("multi-output smoke model attach must return 0", rc == 0);

    VmafPicture ref = {0};
    VmafPicture dist = {0};
    rc = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV400P, 8, 4, 4);
    mu_assert("ref picture alloc must succeed", rc == 0);
    rc = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV400P, 8, 4, 4);
    mu_assert("dist picture alloc must succeed", rc == 0);
    fill_luma(&ref, 64u);
    fill_luma(&dist, 64u);

    rc = vmaf_read_pictures(ctx, &ref, &dist, 0u);
    mu_assert("multi-output tiny model frame run must succeed", rc == 0);

    double mean_score = -1.0;
    double peak_score = -1.0;
    rc = vmaf_feature_score_at_index(ctx, "multi_probe_mean_score", &mean_score, 0u);
    mu_assert("mean_score output must be recorded", rc == 0);
    rc = vmaf_feature_score_at_index(ctx, "multi_probe_peak_score", &peak_score, 0u);
    mu_assert("peak_score output must be recorded", rc == 0);
    mu_assert("mean_score must be positive", mean_score > 0.0);
    mu_assert("peak_score must be positive", peak_score > 0.0);

    rc = vmaf_close(ctx);
    mu_assert("vmaf_close after multi-output tiny model must return 0", rc == 0);
    return NULL;
}

/* --- ADR-0524: symbolic batch dim acceptance ------------------------------ */

/* The shipped NR tiny checkpoints (model/tiny/nr_metric_v1*.onnx) all
 * declare their NCHW input shape as `['batch', 1, 224, 224]` — the
 * ONNX `dim_param='batch'` token, which ORT surfaces through the C
 * API as `-1`. Before ADR-0523 the loader rejected this with -ENOTSUP
 * (errno 95) because `dnn_attach_nchw` required `in_shape[0] == 1`.
 * The fixture `smoke_v0_symbolic_batch.onnx` is the minimal
 * reproducer: a rank-4 Identity graph with a symbolic batch on dim 0
 * and concrete C=1 / H=4 / W=4. Attach must succeed; the per-frame
 * inference loop always emits batch=1 so symbolic batch is safe to
 * fold to 1 at attach time. */
static char *test_attach_accepts_symbolic_batch_rank4(void)
{
    if (!vmaf_dnn_available())
        return NULL;

#ifndef _WIN32
    if (access(SMOKE_SYMBATCH_MODEL, R_OK) != 0)
        return NULL;
#endif

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);

    int rc = vmaf_use_tiny_model(ctx, SMOKE_SYMBATCH_MODEL, NULL);
    mu_assert("ADR-0524: symbolic-batch rank-4 model must attach (folded to batch=1)", rc == 0);

    rc = vmaf_close(ctx);
    mu_assert("vmaf_close after symbolic-batch attach must return 0", rc == 0);
    return NULL;
}

static char *test_rank5_model_closes_session_after_shape_reject(void)
{
    if (!vmaf_dnn_available())
        return NULL;

#ifndef _WIN32
    if (access(RANK5_MODEL, R_OK) != 0)
        return NULL;
#endif

    VmafContext *ctx = alloc_ctx();
    mu_assert("vmaf_init must succeed", ctx != NULL);

    int rc = vmaf_use_tiny_model(ctx, RANK5_MODEL, NULL);
    mu_assert("rank-5 model rejected at tiny attach", rc < 0);

    rc = vmaf_close(ctx);
    mu_assert("vmaf_close after rank-5 reject must return 0", rc == 0);
    return NULL;
}

#ifndef _WIN32
/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Stage a `<stem>.onnx` / `<stem>.int8.onnx` / `<stem>.json` triple in /tmp.
 *
 * Both ONNX copies are the same allowlisted smoke graph — the redirect under
 * test picks a *path*, not a quantisation, so the int8 sibling only has to
 * exist and pass the scanner. Split out of the test body to keep every
 * function inside the `readability-function-size` branch budget. Returns a
 * failure message on a staging error, NULL on success. */
static char *stage_redirect_triple(char *tmpl, char *onnx, char *int8_onnx, char *sidecar,
                                   size_t cap)
{
    const int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    (void)close(fd);

    (void)snprintf(onnx, cap, "%s.onnx", tmpl);
    (void)snprintf(int8_onnx, cap, "%s.int8.onnx", tmpl);
    (void)snprintf(sidecar, cap, "%s.json", tmpl);

    mu_assert("copy smoke onnx failed", copy_file_600(SMOKE_FP32_MODEL, onnx) == 0);
    mu_assert("copy smoke int8 onnx failed", copy_file_600(SMOKE_FP32_MODEL, int8_onnx) == 0);

    static const unsigned char json_dyn[] =
        "{\"kind\":\"fr\",\"quant_mode\":\"dynamic\",\"name\":\"redir_test\"}\n";
    mu_assert("write sidecar failed",
              write_file_600(sidecar, json_dyn, sizeof(json_dyn) - 1u) == 0);
    return NULL;
}

/* Attach @p path to a fresh context and assert the outcome, then close.
 *
 * @p expect_ok non-zero requires `vmaf_use_tiny_model() == 0`, zero requires a
 * negative return. @p message is the failure text of the caller's case; it is
 * plain `char *` because `mu_message_t` is `char *` in C and `mu_assert()`
 * returns it straight out of the enclosing test. */
static char *expect_tiny_attach(const char *path, int expect_ok, char *message)
{
    VmafContext *ctx = alloc_ctx();
    mu_assert("alloc_ctx failed", ctx != NULL);
    const int rc = vmaf_use_tiny_model(ctx, path, NULL);
    mu_assert(message, expect_ok ? (rc == 0) : (rc < 0));
    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_use_tiny_model_int8_redirect_and_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    if (access(SMOKE_FP32_MODEL, R_OK) != 0)
        return NULL;

    char tmpl[] = "/tmp/vmaf-tiny-redir-XXXXXX";
    char onnx[1024];
    char int8_onnx[1024];
    char sidecar[1024];
    char *err = stage_redirect_triple(tmpl, onnx, int8_onnx, sidecar, sizeof(onnx));
    if (err)
        return err;

    /* 1. Both present: redirects to int8 and succeeds. */
    err = expect_tiny_attach(onnx, 1, "redirect to int8 should succeed");
    if (err)
        return err;

    /* 2. int8 removed: falls back to fp32 per ADR-1032 and succeeds. */
    mu_assert("unlink int8 failed", unlink(int8_onnx) == 0);
    err = expect_tiny_attach(onnx, 1, "fallback to fp32 should succeed");
    if (err)
        return err;

    /* 3. Both removed: fails. */
    mu_assert("unlink fp32 failed", unlink(onnx) == 0);
    err = expect_tiny_attach(onnx, 0, "missing both should fail");
    if (err)
        return err;

    (void)unlink(sidecar);
    (void)unlink(tmpl);
    return NULL;
}

static char *test_use_tiny_model_missing_external_data_returns_error_not_abort(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    if (access(TINY_V1_MODEL, R_OK) != 0)
        return NULL;

    char tmpl[] = "/tmp/vmaf-tiny-ext-XXXXXX";
    const int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    (void)close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof(onnx), "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof(sidecar), "%s.json", tmpl);

    /* Copy the ONNX that references external data without copying the
     * companion `.data` file, so the load must fail cleanly. */
    mu_assert("copy vmaf_tiny_v1 failed", copy_file_600(TINY_V1_MODEL, onnx) == 0);
    static const unsigned char json_fp32[] = "{\"kind\":\"fr\",\"quant_mode\":\"fp32\"}\n";
    mu_assert("write sidecar failed",
              write_file_600(sidecar, json_fp32, sizeof(json_fp32) - 1u) == 0);

    char *err =
        expect_tiny_attach(onnx, 0, "missing external data must return error < 0, not abort");
    if (err)
        return err;

    (void)unlink(sidecar);
    (void)unlink(onnx);
    (void)unlink(tmpl);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
#endif

/* -------------------------------------------------------------------------- */

char *run_tests(void)
{
    mu_run_test(test_returns_enosys_when_disabled);
    mu_run_test(test_rejects_null_ctx);
    mu_run_test(test_rejects_null_path);
    mu_run_test(test_rejects_nonexistent_path);
    mu_run_test(test_codec_context_and_resize_reject_bad_args);
#ifndef _WIN32
    mu_run_test(test_rejects_oversized_sidecar_before_ort_open);
    mu_run_test(test_valid_scanner_invalid_ort_model_closes_session);
    mu_run_test(test_invalid_ort_model_frees_loaded_sidecar);
    mu_run_test(test_use_tiny_model_int8_redirect_and_fallback);
    mu_run_test(test_use_tiny_model_missing_external_data_returns_error_not_abort);
#endif
    mu_run_test(test_happy_path_smoke_model);
    mu_run_test(test_attached_multi_output_model_records_named_scores);
    mu_run_test(test_attach_accepts_symbolic_batch_rank4);
    mu_run_test(test_rank5_model_closes_session_after_shape_reject);
    return NULL;
}
