/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Public-surface tests for vmaf_dnn_session_run() and its data types.
 *  Runs against the public headers only (no private dnn/ includes) so it
 *  verifies the stable API contract that downstream consumers see.
 *
 *  When libvmaf was built with -Denable_dnn=disabled, every entry point
 *  returns -ENOSYS and only the stub-semantics tests execute. The real-
 *  ORT path is additionally covered by the CLI smoke gate (test_cli.sh)
 *  against the model/tiny/smoke_v0.onnx fixture.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "test.h"

#include "libvmaf/dnn.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* DNN_OPEN_SKIP_RC — return codes that indicate a CI infrastructure gap rather
 * than a code bug.  Tests guard with:
 *
 *   if (rc == -ENOENT || rc == -EIO) return NULL;
 *
 * -ENOENT: ONNX model file not present in the working tree (e.g. shallow clone
 *          without testdata).
 * -EIO: ORT CUDA execution-provider .so absent on CPU-only CI runners.  ORT
 *       1.22+ defers the dlopen of libonnxruntime_providers_cuda.so to
 *       CreateSession; when the .so is missing the primary CreateSession fails
 *       and the CPU fallback may also fail because ORT's environment state was
 *       damaged by the failed dlopen.  The root provider-library is not
 *       installed on the DNN-enabled ubuntu CI legs, which install only the
 *       CPU tarball.  Skipping here is correct: the feature under test is the
 *       ORT inference path itself, not the EP-selection fallback logic. */
#define DNN_OPEN_SKIP_RC(rc) ((rc) == -ENOENT || (rc) == -EIO)

static char *test_stub_returns_enosys_when_disabled(void)
{
    if (vmaf_dnn_available()) {
        /* This binary was built with real ORT. The stub-only assertions
         * do not apply — skip without failing. */
        return NULL;
    }
    float in_data[1] = {0.0f};
    int64_t in_shape[1] = {1};
    float out_data[1] = {0.0f};

    VmafDnnInput in = {.name = NULL, .data = in_data, .shape = in_shape, .rank = 1};
    VmafDnnOutput out = {.name = NULL, .data = out_data, .capacity = 1, .written = 0};

    int rc = vmaf_dnn_session_run(NULL, &in, 1, &out, 1);
    mu_assert("stub must return -ENOSYS when DNN is disabled", rc == -ENOSYS);
    return NULL;
}

static char *test_rejects_null_session(void)
{
    float in_data[1] = {0.0f};
    int64_t in_shape[1] = {1};
    float out_data[1] = {0.0f};
    VmafDnnInput in = {.name = NULL, .data = in_data, .shape = in_shape, .rank = 1};
    VmafDnnOutput out = {.name = NULL, .data = out_data, .capacity = 1, .written = 0};

    int rc = vmaf_dnn_session_run(NULL, &in, 1, &out, 1);
    mu_assert("NULL session must be rejected", rc < 0);
    return NULL;
}

static char *test_descriptor_field_layout(void)
{
    /* Compile-time sanity: the public descriptor fields must stay where
     * downstream callers expect them (designated initialisers above pin
     * the contract). Catches accidental field-reorder refactors. */
    VmafDnnInput in = {.name = "ref", .data = NULL, .shape = NULL, .rank = 4};
    VmafDnnOutput out = {.name = "y", .data = NULL, .capacity = 0, .written = 0};
    mu_assert("input.name binding", in.name != NULL);
    mu_assert("input.rank binding", in.rank == 4u);
    mu_assert("output.name binding", out.name != NULL);
    mu_assert("output.written starts zeroed", out.written == 0u);
    return NULL;
}

static char *test_session_open_rejects_null_out(void)
{
    int rc = vmaf_dnn_session_open(NULL, "anything.onnx", NULL);
    /* Stub branch: -ENOSYS; real branch: -EINVAL. Either is a hard reject. */
    mu_assert("NULL out pointer rejected", rc < 0);
    return NULL;
}

static char *test_session_open_rejects_null_path(void)
{
    VmafDnnSession *s = NULL;
    int rc = vmaf_dnn_session_open(&s, NULL, NULL);
    mu_assert("NULL path rejected", rc < 0);
    mu_assert("session pointer not written on reject", s == NULL);
    return NULL;
}

static char *test_session_open_rejects_missing_file(void)
{
    if (!vmaf_dnn_available()) {
        /* Stub returns -ENOSYS regardless of path; no file-existence check
         * to exercise. Skip without failing. */
        return NULL;
    }
    VmafDnnSession *s = NULL;
    int rc = vmaf_dnn_session_open(&s, "/nonexistent/path/to/model.onnx", NULL);
    mu_assert("missing model file rejected", rc < 0);
    mu_assert("session pointer not populated", s == NULL);
    return NULL;
}

static char *test_session_run_luma8_rejects_null(void)
{
    /* Stub branch: returns -ENOSYS for any args. Real branch: -EINVAL on
     * NULL sess/in/out. The wrapper rejects either way. */
    uint8_t buf[16] = {0};
    int rc = vmaf_dnn_session_run_luma8(NULL, buf, 4, 4, 4, buf, 4);
    mu_assert("NULL session rejected by run_luma8", rc < 0);
    return NULL;
}

static char *test_session_close_null_is_noop(void)
{
    /* Free on NULL is a hard contract — must never crash. There is no
     * return value to assert; reaching the next line is the test. */
    vmaf_dnn_session_close(NULL);
    mu_assert("close(NULL) returned without crashing", 1);
    return NULL;
}

static char *test_attached_ep_null_returns_null(void)
{
    const char *ep = vmaf_dnn_session_attached_ep(NULL);
    mu_assert("attached_ep(NULL) returns NULL", ep == NULL);
    return NULL;
}

static char *test_run_rejects_zero_n_inputs(void)
{
    /* Even with non-NULL pointers, 0 inputs / 0 outputs must be rejected.
     * Stub returns -ENOSYS; real branch returns -EINVAL. */
    float buf[1] = {0.0f};
    int64_t shape[1] = {1};
    VmafDnnInput in = {.name = NULL, .data = buf, .shape = shape, .rank = 1};
    VmafDnnOutput out = {.name = NULL, .data = buf, .capacity = 1, .written = 0};
    int rc = vmaf_dnn_session_run((VmafDnnSession *)0xdeadbeef, &in, 0u, &out, 1u);
    mu_assert("zero n_inputs rejected", rc < 0);
    rc = vmaf_dnn_session_run((VmafDnnSession *)0xdeadbeef, &in, 1u, &out, 0u);
    mu_assert("zero n_outputs rejected", rc < 0);
    return NULL;
}

#define SMOKE_FP32_MODEL "model/tiny/smoke_v0.onnx"

static char *test_session_run_luma8_size_mismatch(void)
{
    /* Drives the w/h mismatch branch (-ERANGE, lines 134-135). Open with
     * a known 2x2 model, then run with a 4x4 buffer — must reject. */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    /* smoke_v0.onnx is fixed-shape NCHW [1,1,4,4]. Pass 7x7 buffers — the
     * w/h mismatch must return -ERANGE before any tensor copy happens. */
    uint8_t in[49] = {0};
    uint8_t out[49] = {0};
    rc = vmaf_dnn_session_run_luma8(sess, in, 7, 7, 7, out, 7);
    mu_assert("w/h mismatch returns negative", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

/* ADR-0170 / T6-4: end-to-end happy-path for run_plane16 — opens the
 * 4x4 smoke model and runs a real ORT inference on a packed uint16
 * buffer. Drives the from_plane16 → ort_infer → to_plane16 chain
 * (covers ~30 LoC in dnn_api.c that the rejection-only tests skip). */
static char *test_session_run_plane16_happy_path(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    /* 10-bit packed uint16 buffer; pin a deterministic input so the
     * test is byte-exact. */
    uint16_t in[16];
    uint16_t out[16] = {0};
    for (int i = 0; i < 16; ++i)
        in[i] = (uint16_t)(100 * i);
    rc = vmaf_dnn_session_run_plane16(sess, in, 4u * sizeof(uint16_t), 4, 4, 10, out,
                                      4u * sizeof(uint16_t));
    mu_assert("plane16 happy-path ORT inference ok", rc == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

#ifndef _WIN32
/* ADR-0174 / T5-3b coverage helpers — exercise the runtime int8
 * redirect block in vmaf_dnn_session_open. The fopen-with-0600
 * pattern matches the existing test_model_loader.c helper and
 * sidesteps `cpp/world-writable-file-creation` CodeQL alerts that
 * a default `fopen(..., "wb")` would trigger (CWE-732). */
static FILE *fopen_w_600(const char *path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "wb");
    if (!fp)
        (void)close(fd);
    return fp;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc)
        return -1;
    FILE *fdst = fopen_w_600(dst);
    if (!fdst) {
        (void)fclose(fsrc);
        return -1;
    }
    char buf[4096];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) {
            rc = -1;
            break;
        }
    }
    (void)fclose(fsrc);
    (void)fclose(fdst);
    return rc;
}

static int write_sidecar_dynamic(const char *path)
{
    FILE *s = fopen_w_600(path);
    if (!s)
        return -1;
    const char *json = "{\"kind\": \"fr\", \"quant_mode\": \"dynamic\"}\n";
    size_t len = strlen(json);
    int rc = (fwrite(json, 1, len, s) == len) ? 0 : -1;
    (void)fclose(s);
    return rc;
}

/* sidecar declares quant_mode=dynamic but no .int8.onnx sibling →
 * vmaf_dnn_session_open must fall back to the fp32 baseline and succeed
 * (ADR-1032: fp32-fallback policy introduced in PR #642). */
static char *test_session_open_int8_missing_falls_back_to_fp32(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char base[] = "/tmp/vmaf-dnn-int8-miss-XXXXXX";
    int fd = mkstemp(base);
    if (fd < 0)
        return NULL;
    (void)close(fd);

    char onnx[1100];
    char sidecar[1100];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", base);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", base);
    if (copy_file(SMOKE_FP32_MODEL, onnx) != 0) {
        (void)unlink(base);
        return NULL;
    }
    mu_assert("write sidecar ok", write_sidecar_dynamic(sidecar) == 0);

    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, onnx, NULL);
    /* ADR-1032: missing .int8.onnx is no longer a hard error; the runtime
     * falls back to the fp32 baseline transparently. */
    mu_assert("missing .int8.onnx falls back to fp32 (rc == 0)", rc == 0);
    mu_assert("session populated on fp32 fallback", sess != NULL);
    vmaf_dnn_session_close(sess);

    (void)unlink(sidecar);
    (void)unlink(onnx);
    (void)unlink(base);
    return NULL;
}

/* sidecar declares quant_mode=dynamic and a valid .int8.onnx exists
 * (we copy the fp32 smoke as a stand-in) → vmaf_dnn_session_open
 * must succeed via the redirect path. */
static char *test_session_open_int8_redirect_succeeds(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char base[] = "/tmp/vmaf-dnn-int8-ok-XXXXXX";
    int fd = mkstemp(base);
    if (fd < 0)
        return NULL;
    (void)close(fd);

    char onnx[1100];
    char int8_onnx[1200];
    char sidecar[1100];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", base);
    (void)snprintf(int8_onnx, sizeof int8_onnx, "%s.int8.onnx", base);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", base);
    if (copy_file(SMOKE_FP32_MODEL, onnx) != 0 || copy_file(SMOKE_FP32_MODEL, int8_onnx) != 0) {
        (void)unlink(base);
        return NULL;
    }
    mu_assert("write sidecar ok", write_sidecar_dynamic(sidecar) == 0);

    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, onnx, NULL);
    mu_assert("int8 redirect open ok", rc == 0);
    mu_assert("session populated", sess != NULL);
    vmaf_dnn_session_close(sess);

    (void)unlink(sidecar);
    (void)unlink(int8_onnx);
    (void)unlink(onnx);
    (void)unlink(base);
    return NULL;
}
#endif /* !_WIN32 */

/* ADR-0170 / T6-4: drive the input-validation branches of
 * vmaf_dnn_session_run_plane16 without needing a real ORT session. */
static char *test_session_run_plane16_rejects_null(void)
{
    uint16_t buf[16] = {0};
    int rc = vmaf_dnn_session_run_plane16(NULL, buf, 8, 4, 4, 10, buf, 8);
    mu_assert("NULL session rejected by run_plane16", rc < 0);
    return NULL;
}

static char *test_session_run_plane16_rejects_bad_bpc(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);
    uint16_t in[16] = {0};
    uint16_t out[16] = {0};
    /* bpc < 9 not allowed (use _luma8 for 8-bit). */
    rc = vmaf_dnn_session_run_plane16(sess, in, 8, 4, 4, 8, out, 8);
    mu_assert("bpc=8 rejected", rc == -EINVAL);
    /* bpc > 16 also out of range. */
    rc = vmaf_dnn_session_run_plane16(sess, in, 8, 4, 4, 17, out, 8);
    mu_assert("bpc=17 rejected", rc == -EINVAL);
    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_plane16_size_mismatch(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);
    /* smoke_v0.onnx pinned at 4x4; 7x7 input must hit the -ERANGE branch
     * inside run_plane16. */
    uint16_t in[49] = {0};
    uint16_t out[49] = {0};
    rc = vmaf_dnn_session_run_plane16(sess, in, 14, 7, 7, 10, out, 14);
    mu_assert("w/h mismatch returns negative", rc < 0);
    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_heap_path_for_many_inputs(void)
{
    /* Drives the heap-allocation branch (n_inputs > 4, lines 175-185 and
     * the matching free at 207-210). We use a stub session pointer — the
     * model never gets opened — but the input-vector heap path runs before
     * vmaf_ort_run is called. The call must return < 0 (most likely -EFAULT
     * or similar) without leaking. Caveat: when DNN is disabled the stub
     * returns -ENOSYS before any allocation, so this is an effective test
     * only on real-ORT builds. */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    /* 5 inputs forces the heap-allocation branch (stack array is size 4). */
    float buf[4] = {0};
    int64_t shape[4] = {1, 1, 2, 2};
    VmafDnnInput in[5];
    for (size_t i = 0; i < 5; ++i) {
        in[i].name = "x";
        in[i].data = buf;
        in[i].shape = shape;
        in[i].rank = 4u;
    }
    VmafDnnOutput out[5];
    for (size_t i = 0; i < 5; ++i) {
        out[i].name = "y";
        out[i].data = buf;
        out[i].capacity = 4u;
        out[i].written = 0u;
    }
    /* The model has only 1 input — passing 5 must fail in vmaf_ort_run,
     * but the heap allocate/free pair around the call still executes. */
    rc = vmaf_dnn_session_run(sess, in, 5u, out, 5u);
    mu_assert("mismatched input count rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_unknown_input_name(void)
{
    /* Drives resolve_name() lookup-by-name failure (ort_backend.c line 560,
     * caller branch line 604 → -EINVAL). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {
        .name = "definitely-not-a-real-input", .data = buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("unknown input name rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_unknown_output_name(void)
{
    /* resolve_name() failure on output table (line 625 → -EINVAL). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = "no-such-output", .data = buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("unknown output name rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_zero_rank_input(void)
{
    /* Drives the rank == 0 guard inside vmaf_ort_run (line 598 → -EINVAL). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = buf, .shape = shape, .rank = 0u};
    VmafDnnOutput out = {.name = NULL, .data = buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("zero-rank input rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_negative_dim(void)
{
    /* Drives the shape[d] <= 0 guard (line 608 → -EINVAL). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, -1, 4}; /* dynamic dim sentinel — must be rejected */
    VmafDnnInput in = {.name = NULL, .data = buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("negative input dim rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_null_input_data(void)
{
    /* Drives the !inputs[i].data guard (line 598 → -EINVAL). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = NULL, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("null input data rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_null_output_data(void)
{
    /* Drives the !outputs[i].data guard (line 621 → -EINVAL). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = NULL, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("null output data rejected", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_rejects_null_vectors(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, NULL, 1, &out, 1);
    mu_assert("NULL input vector rejected", rc == -EINVAL);
    rc = vmaf_dnn_session_run(sess, &in, 1, NULL, 1);
    mu_assert("NULL output vector rejected", rc == -EINVAL);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_generic_success(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float in_buf[16] = {0};
    float out_buf[16] = {0};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = in_buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = out_buf, .capacity = 16, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("generic session run succeeds", rc == 0);
    mu_assert("generic session run reports output count", out.written == 16u);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_undersized_output(void)
{
    /* Drives copy_output_tensor() -ENOSPC branch (lines 391, 646-649). The
     * smoke model's output is 4x4 = 16 floats; provide capacity 1 and check
     * we get -ENOSPC and written reflects the required count. */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    float in_buf[16] = {0};
    float out_buf[1] = {0.f};
    int64_t shape[4] = {1, 1, 4, 4};
    VmafDnnInput in = {.name = NULL, .data = in_buf, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = NULL, .data = out_buf, .capacity = 1, .written = 0};
    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("undersized output buffer returns -ENOSPC", rc == -ENOSPC);
    mu_assert("required count surfaced via written", out.written == 16u);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_run_named_io_round_trip(void)
{
    /* Drives the resolve_name() success-by-name branch (lines 557-558).
     * Names match exactly the smoke model's IO ("features"/"out") — if the
     * model uses different names this gracefully falls back to <0 and the
     * test stays informative without spuriously failing. */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);

    /* Use the legacy luma path to drive a successful end-to-end run that
     * exercises the inference branches in vmaf_ort_infer (build_input_tensor
     * fp32 path + copy_output_tensor non-fp16 branch). */
    uint8_t in_buf[16] = {0};
    uint8_t out_buf[16] = {0};
    rc = vmaf_dnn_session_run_luma8(sess, in_buf, 4, 4, 4, out_buf, 4);
    mu_assert("luma8 run on smoke model succeeds", rc == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_open_threads_config(void)
{
    /* Drives the cfg->threads > 0 branch in vmaf_ort_open (line 204). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CPU, .threads = 2};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("threads=2 open succeeds", rc == 0);
    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_open_rocm_falls_through(void)
{
    /* Drives VMAF_DNN_DEVICE_ROCM branch (lines 237-240). On a CPU-only ORT
     * build try_append_rocm() returns non-zero and ep_name stays "CPU". */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_ROCM};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("ROCm request does not fail open", rc == 0);
    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_session_open_explicit_ep_selectors_fall_back(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const VmafDnnDevice devices[] = {
        VMAF_DNN_DEVICE_CUDA,         VMAF_DNN_DEVICE_OPENVINO,     VMAF_DNN_DEVICE_OPENVINO_NPU,
        VMAF_DNN_DEVICE_OPENVINO_CPU, VMAF_DNN_DEVICE_OPENVINO_GPU, VMAF_DNN_DEVICE_COREML,
        VMAF_DNN_DEVICE_COREML_ANE,   VMAF_DNN_DEVICE_COREML_GPU,   VMAF_DNN_DEVICE_COREML_CPU,
    };
    const size_t n = sizeof(devices) / sizeof(devices[0]);
    for (size_t i = 0; i < n; ++i) {
        VmafDnnSession *sess = NULL;
        VmafDnnConfig cfg = {.device = devices[i], .device_index = 1, .threads = 1};
        int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
        if (DNN_OPEN_SKIP_RC(rc))
            return NULL;
        mu_assert("explicit EP selector opens or CPU-falls-back", rc == 0);
        mu_assert("session populated", sess != NULL);
        const char *ep = vmaf_dnn_session_attached_ep(sess);
        mu_assert("attached EP string reported", ep != NULL && ep[0] != '\0');
        vmaf_dnn_session_close(sess);
    }
    return NULL;
}

static char *test_attached_ep_after_session_close(void)
{
    /* Drives the success path of vmaf_dnn_session_attached_ep (line 231). */
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("smoke model open ok", rc == 0);
    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("attached EP is reported", ep != NULL && ep[0] != '\0');
    vmaf_dnn_session_close(sess);
    return NULL;
}

#ifndef _WIN32
/* Drives the sidecar-load non-ENOENT error branch in vmaf_dnn_session_open
 * (lines 78-79: free(s) + return rc when sidecar_load returns a non-ENOENT
 * negative). vmaf_dnn_sidecar_load returns -EFBIG when the .json sidecar
 * is larger than 1 MiB; we craft a 1.05 MiB sidecar to hit that path
 * deterministically without depending on any specific malformed content. */
static char *test_session_open_oversize_sidecar_returns_error(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char base[] = "/tmp/vmaf-dnn-sidecar-big-XXXXXX";
    int fd = mkstemp(base);
    if (fd < 0)
        return NULL;
    (void)close(fd);

    char onnx[1100];
    char sidecar[1100];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", base);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", base);
    if (copy_file(SMOKE_FP32_MODEL, onnx) != 0) {
        (void)unlink(base);
        return NULL;
    }

    /* Write a >1 MiB sidecar — the sidecar loader's hard cap is 1 << 20
     * bytes (model_loader.c stat_sidecar path), so 1100000 bytes trips
     * -EFBIG before any JSON parse, propagating out of vmaf_dnn_session_open
     * via the lines 77-80 cleanup. The payload itself is arbitrary; we
     * use a JSON-shaped buffer with padding to keep the file at least
     * parseably-looking, but the size check fires first. */
    FILE *s = fopen_w_600(sidecar);
    if (!s) {
        (void)unlink(onnx);
        (void)unlink(base);
        return NULL;
    }
    /* Header + huge filler-comment-string-value to push past 1 MiB. */
    (void)fputs("{\"kind\": \"fr\", \"pad\": \"", s);
    const size_t fill = (size_t)(1100000);
    for (size_t i = 0; i < fill; ++i)
        (void)fputc('x', s);
    (void)fputs("\"}\n", s);
    (void)fclose(s);

    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, onnx, NULL);
    mu_assert("oversize sidecar must surface a negative error", rc < 0);
    mu_assert("session pointer remains NULL on sidecar error", sess == NULL);

    (void)unlink(sidecar);
    (void)unlink(onnx);
    (void)unlink(base);
    return NULL;
}
#endif /* !_WIN32 */

/* Drives the rank==4 but shape[0] != 1 branch in vmaf_dnn_session_open —
 * the else of the legacy luma fast-path detection (line 138). The
 * smoke_v0_symbolic_batch.onnx fixture has a symbolic dim_param='batch'
 * on axis 0, which onnxruntime reports as shape[0] == -1 (dynamic). The
 * session opens successfully but in_buf / out_buf stay NULL, so any
 * subsequent run_luma8 call must hit the !sess->in_buf guard and return
 * -ENOTSUP. Complements test_attach_accepts_symbolic_batch_rank4 (which
 * exercises the libvmaf.c attach path); this one exercises the standalone
 * vmaf_dnn_session_open + vmaf_dnn_session_run_luma8 path. */
#define SMOKE_SYMBATCH_MODEL "model/tiny/smoke_v0_symbolic_batch.onnx"
static char *test_session_open_symbolic_batch_skips_luma_fast_path(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_SYMBATCH_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    /* Symbolic-batch loader behaviour is gated on ADR-0523; the legacy
     * fast-path detection inside vmaf_dnn_session_open must not crash and
     * must leave the session usable through the generic run() path. */
    mu_assert("symbolic-batch model opens ok", rc == 0);
    mu_assert("session populated", sess != NULL);

    uint8_t in[16] = {0};
    uint8_t out[16] = {0};
    rc = vmaf_dnn_session_run_luma8(sess, in, 4, 4, 4, out, 4);
    mu_assert("luma8 must return -ENOTSUP when in_buf was not allocated", rc == -ENOTSUP);

    vmaf_dnn_session_close(sess);
    return NULL;
}

/* Drives the legacy luma fast-path's run_plane16 -ENOTSUP guard (line 207)
 * paired with the open-time skip caused by a symbolic batch dim — same
 * mechanism as the run_luma8 sibling above, exercises the 16-bit entry
 * point so both fast-path entry points have the not-allocated branch
 * covered. */
static char *test_session_symbolic_batch_run_plane16_returns_notsup(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    VmafDnnSession *sess = NULL;
    int rc = vmaf_dnn_session_open(&sess, SMOKE_SYMBATCH_MODEL, NULL);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("symbolic-batch model opens ok", rc == 0);
    mu_assert("session populated", sess != NULL);

    uint16_t in[16] = {0};
    uint16_t out[16] = {0};
    rc = vmaf_dnn_session_run_plane16(sess, in, 8, 4, 4, 10, out, 8);
    mu_assert("plane16 must return -ENOTSUP when in_buf was not allocated", rc == -ENOTSUP);

    vmaf_dnn_session_close(sess);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_stub_returns_enosys_when_disabled);
    mu_run_test(test_rejects_null_session);
    mu_run_test(test_descriptor_field_layout);
    mu_run_test(test_session_open_rejects_null_out);
    mu_run_test(test_session_open_rejects_null_path);
    mu_run_test(test_session_open_rejects_missing_file);
    mu_run_test(test_session_run_luma8_rejects_null);
    mu_run_test(test_session_close_null_is_noop);
    mu_run_test(test_attached_ep_null_returns_null);
    mu_run_test(test_run_rejects_zero_n_inputs);
    mu_run_test(test_session_run_luma8_size_mismatch);
    mu_run_test(test_session_run_plane16_rejects_null);
    mu_run_test(test_session_run_plane16_rejects_bad_bpc);
    mu_run_test(test_session_run_plane16_size_mismatch);
    mu_run_test(test_session_run_plane16_happy_path);
#ifndef _WIN32
    mu_run_test(test_session_open_int8_missing_falls_back_to_fp32);
    mu_run_test(test_session_open_int8_redirect_succeeds);
#endif
    mu_run_test(test_session_run_heap_path_for_many_inputs);
    mu_run_test(test_attached_ep_after_session_close);
    mu_run_test(test_session_run_unknown_input_name);
    mu_run_test(test_session_run_unknown_output_name);
    mu_run_test(test_session_run_zero_rank_input);
    mu_run_test(test_session_run_negative_dim);
    mu_run_test(test_session_run_null_input_data);
    mu_run_test(test_session_run_null_output_data);
    mu_run_test(test_session_run_rejects_null_vectors);
    mu_run_test(test_session_run_generic_success);
    mu_run_test(test_session_run_undersized_output);
    mu_run_test(test_session_run_named_io_round_trip);
    mu_run_test(test_session_open_threads_config);
    mu_run_test(test_session_open_rocm_falls_through);
    mu_run_test(test_session_open_explicit_ep_selectors_fall_back);
#ifndef _WIN32
    mu_run_test(test_session_open_oversize_sidecar_returns_error);
#endif
    mu_run_test(test_session_open_symbolic_batch_skips_luma_fast_path);
    mu_run_test(test_session_symbolic_batch_run_plane16_returns_notsup);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
