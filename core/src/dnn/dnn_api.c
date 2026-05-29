/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Standalone `libvmaf/dnn.h` entry points: session open / run / close,
 *  capability probe, and tensor helpers. None of these reference
 *  `libvmaf.c`'s internal state, so the TU is safe to link into the
 *  standalone unit-test binaries (which use the session API via
 *  `feature_lpips.c` but do not link `libvmaf.c`).
 *
 *  The `vmaf_use_tiny_model` ctx-attach entry point lives in
 *  `dnn_attach_api.c` because it depends on `vmaf_ctx_dnn_attach` from
 *  `libvmaf.c`.
 *
 *  When built with -Denable_dnn=false, this TU compiles a stub that returns
 *  -ENOSYS from every entry point so consumers degrade gracefully.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/dnn.h"
#include "libvmaf/vmaf_assert.h"

#include "model_loader.h"
#include "ort_backend.h"
#include "tensor_io.h"

int vmaf_dnn_available(void)
{
#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
    return 1;
#else
    return 0;
#endif
}

#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN

struct VmafDnnSession {
    VmafOrtSession *ort;
    VmafModelSidecar meta;
    bool has_sidecar;
    int w;
    int h;
    float *in_buf;  /* w*h floats */
    float *out_buf; /* w*h floats */
};

int vmaf_dnn_session_open(VmafDnnSession **out, const char *onnx_path, const VmafDnnConfig *cfg)
{
    if (!out || !onnx_path)
        return -EINVAL;
    assert(out != NULL);
    assert(onnx_path != NULL);

    /* T7-12: the historical VMAF_MAX_MODEL_BYTES env override has been
     * removed. The compile-time cap (VMAF_DNN_DEFAULT_MAX_BYTES = 50 MB)
     * is the single source of truth — two release cycles passed without
     * a shipped model approaching the cap, so the testing-hatch is
     * retired. */
    const size_t max_bytes = VMAF_DNN_DEFAULT_MAX_BYTES;
    int rc = vmaf_dnn_validate_onnx(onnx_path, max_bytes);
    if (rc < 0)
        return rc;

    VmafDnnSession *s = (VmafDnnSession *)calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;

    rc = vmaf_dnn_sidecar_load(onnx_path, &s->meta);
    if (rc == 0)
        s->has_sidecar = true;
    else if (rc != -ENOENT) {
        free(s);
        return rc;
    }

    /* ADR-0174 / T5-3b: when the sidecar declares quant_mode != FP32, the
     * caller-supplied path is the fp32 baseline; the runtime should load
     * the sibling `<basename>.int8.onnx` instead. We append `.int8.onnx`
     * to the basename (stripping a trailing `.onnx`) and re-run the
     * size + allowlist validator on the int8 file. The fp32 file stays
     * on disk as the regression baseline. */
    const char *load_path = onnx_path;
    char int8_path[4096];
    if (s->has_sidecar && s->meta.quant_mode != VMAF_QUANT_FP32) {
        size_t plen = strlen(onnx_path);
        const char *suffix = ".onnx";
        const size_t suffix_len = 5u;
        size_t base_len =
            (plen >= suffix_len && strcmp(onnx_path + plen - suffix_len, suffix) == 0) ?
                plen - suffix_len :
                plen;
        if (base_len + sizeof(".int8.onnx") > sizeof(int8_path)) {
            if (s->has_sidecar)
                vmaf_dnn_sidecar_free(&s->meta);
            free(s);
            return -ENAMETOOLONG;
        }
        memcpy(int8_path, onnx_path, base_len);
        memcpy(int8_path + base_len, ".int8.onnx", sizeof(".int8.onnx"));
        rc = vmaf_dnn_validate_onnx(int8_path, max_bytes);
        if (rc < 0) {
            /* int8 file missing or fails the allowlist — fall back to
             * fp32 with a debug log; better degraded than dead. */
            if (s->has_sidecar)
                vmaf_dnn_sidecar_free(&s->meta);
            free(s);
            return rc;
        }
        load_path = int8_path;
    }

    rc = vmaf_ort_open(&s->ort, load_path, cfg);
    if (rc < 0) {
        if (s->has_sidecar)
            vmaf_dnn_sidecar_free(&s->meta);
        free(s);
        return rc;
    }

    int64_t shape[4] = {0};
    size_t rank = 0;
    rc = vmaf_ort_input_shape(s->ort, shape, 4u, &rank);
    if (rc < 0)
        goto fail;

    /* Preserve the legacy luma fast-path when the model's input shape is
     * NCHW [1,1,H,W]: allocate scratch buffers sized for luma so
     * vmaf_dnn_session_run_luma8() can avoid reallocating per-frame. For
     * any other shape we leave in_buf / out_buf NULL and w == h == 0;
     * vmaf_dnn_session_run_luma8() then returns -ENOTSUP, while the
     * generic vmaf_dnn_session_run() path works regardless. */
    if (rank == 4 && shape[0] == 1 && shape[1] == 1 && shape[2] > 0 && shape[3] > 0) {
        s->h = (int)shape[2];
        s->w = (int)shape[3];
        const size_t n = (size_t)s->w * (size_t)s->h;
        s->in_buf = (float *)calloc(n, sizeof(float));
        s->out_buf = (float *)calloc(n, sizeof(float));
        if (!s->in_buf || !s->out_buf) {
            rc = -ENOMEM;
            goto fail;
        }
    }

    *out = s;
    return 0;

fail:
    vmaf_dnn_session_close(s);
    return rc;
}

int vmaf_dnn_session_run_luma8(VmafDnnSession *sess, const uint8_t *in, size_t in_stride, int w,
                               int h, uint8_t *out, size_t out_stride)
{
    if (!sess || !in || !out)
        return -EINVAL;
    /* in_buf / out_buf are only allocated when the model's input shape is
     * NCHW [1,1,H,W] (see vmaf_dnn_session_open). Models with multi-
     * channel or multi-input graphs must use vmaf_dnn_session_run(). */
    if (!sess->in_buf || !sess->out_buf || sess->w == 0 || sess->h == 0)
        return -ENOTSUP;
    if (w != sess->w || h != sess->h)
        return -ERANGE;

    const float *mean = NULL;
    const float *std = NULL;
    float m = 0.f, s_ = 1.f;
    if (sess->has_sidecar && sess->meta.has_norm && sess->meta.norm_std > 0.f) {
        m = sess->meta.norm_mean;
        s_ = sess->meta.norm_std;
        mean = &m;
        std = &s_;
    }

    int rc = vmaf_tensor_from_luma(in, in_stride, w, h, VMAF_TENSOR_LAYOUT_NCHW,
                                   VMAF_TENSOR_DTYPE_F32, mean, std, sess->in_buf);
    if (rc < 0)
        return rc;

    const int64_t shape[4] = {1, 1, h, w};
    const size_t n = (size_t)w * (size_t)h;
    size_t written = 0;
    rc = vmaf_ort_infer(sess->ort, sess->in_buf, shape, 4, sess->out_buf, n, &written);
    if (rc < 0)
        return rc;
    if (written != n)
        return -ENOTSUP;

    return vmaf_tensor_to_luma(sess->out_buf, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, w, h,
                               mean, std, out, out_stride);
}

int vmaf_dnn_session_run_plane16(VmafDnnSession *sess, const uint16_t *in, size_t in_stride, int w,
                                 int h, int bpc, uint16_t *out, size_t out_stride)
{
    if (!sess || !in || !out)
        return -EINVAL;
    if (bpc < 9 || bpc > 16)
        return -EINVAL;
    if (!sess->in_buf || !sess->out_buf || sess->w == 0 || sess->h == 0)
        return -ENOTSUP;
    if (w != sess->w || h != sess->h)
        return -ERANGE;

    const float *mean = NULL;
    const float *std = NULL;
    float m = 0.f, s_ = 1.f;
    if (sess->has_sidecar && sess->meta.has_norm && sess->meta.norm_std > 0.f) {
        m = sess->meta.norm_mean;
        s_ = sess->meta.norm_std;
        mean = &m;
        std = &s_;
    }

    int rc = vmaf_tensor_from_plane16(in, in_stride, w, h, bpc, VMAF_TENSOR_LAYOUT_NCHW,
                                      VMAF_TENSOR_DTYPE_F32, mean, std, sess->in_buf);
    if (rc < 0)
        return rc;

    const int64_t shape[4] = {1, 1, h, w};
    const size_t n = (size_t)w * (size_t)h;
    size_t written = 0;
    rc = vmaf_ort_infer(sess->ort, sess->in_buf, shape, 4, sess->out_buf, n, &written);
    if (rc < 0)
        return rc;
    if (written != n)
        return -ENOTSUP;

    return vmaf_tensor_to_plane16(sess->out_buf, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, w,
                                  h, bpc, mean, std, out, out_stride);
}

int vmaf_dnn_session_run(VmafDnnSession *sess, const VmafDnnInput *inputs, size_t n_inputs,
                         VmafDnnOutput *outputs, size_t n_outputs)
{
    if (!sess || !inputs || !outputs || n_inputs == 0u || n_outputs == 0u)
        return -EINVAL;
    assert(sess != NULL);
    assert(sess->ort != NULL);

    VmafOrtTensorIn stack_in[4];
    VmafOrtTensorOut stack_out[4];
    VmafOrtTensorIn *ti =
        (n_inputs <= 4u) ? stack_in : (VmafOrtTensorIn *)calloc(n_inputs, sizeof(VmafOrtTensorIn));
    VmafOrtTensorOut *to = (n_outputs <= 4u) ?
                               stack_out :
                               (VmafOrtTensorOut *)calloc(n_outputs, sizeof(VmafOrtTensorOut));
    if (!ti || !to) {
        if (ti && ti != stack_in)
            free(ti);
        if (to && to != stack_out)
            free(to);
        return -ENOMEM;
    }

    for (size_t i = 0; i < n_inputs; ++i) {
        ti[i].name = inputs[i].name;
        ti[i].data = inputs[i].data;
        ti[i].shape = inputs[i].shape;
        ti[i].rank = inputs[i].rank;
    }
    for (size_t i = 0; i < n_outputs; ++i) {
        to[i].name = outputs[i].name;
        to[i].data = outputs[i].data;
        to[i].capacity = outputs[i].capacity;
        to[i].written = 0u;
    }

    int rc = vmaf_ort_run(sess->ort, ti, n_inputs, to, n_outputs);

    for (size_t i = 0; i < n_outputs; ++i) {
        outputs[i].written = to[i].written;
    }

    if (ti != stack_in)
        free(ti);
    if (to != stack_out)
        free(to);
    return rc;
}

void vmaf_dnn_session_close(VmafDnnSession *sess)
{
    if (!sess)
        return;
    if (sess->ort)
        vmaf_ort_close(sess->ort);
    if (sess->has_sidecar)
        vmaf_dnn_sidecar_free(&sess->meta);
    free(sess->in_buf);
    free(sess->out_buf);
    free(sess);
}

const char *vmaf_dnn_session_attached_ep(VmafDnnSession *sess)
{
    if (!sess || !sess->ort)
        return NULL;
    return vmaf_ort_attached_ep(sess->ort);
}

#else /* !VMAF_HAVE_DNN */

struct VmafDnnSession {
    int _unused;
};

int vmaf_dnn_session_open(VmafDnnSession **out, const char *onnx_path, const VmafDnnConfig *cfg)
{
    (void)out;
    (void)onnx_path;
    (void)cfg;
    return -ENOSYS;
}

/* Stub signature must match the real-ORT path declared in the header. */
int vmaf_dnn_session_run_luma8(VmafDnnSession *sess, const uint8_t *in, size_t in_stride, int w,
                               int h, uint8_t *out,
                               size_t out_stride) // NOLINT(readability-non-const-parameter)
{
    (void)sess;
    (void)in;
    (void)in_stride;
    (void)w;
    (void)h;
    (void)out;
    (void)out_stride;
    return -ENOSYS;
}

int vmaf_dnn_session_run_plane16(VmafDnnSession *sess, const uint16_t *in, size_t in_stride, int w,
                                 int h, int bpc,
                                 uint16_t *out, // NOLINT(readability-non-const-parameter)
                                 size_t out_stride)
{
    (void)sess;
    (void)in;
    (void)in_stride;
    (void)w;
    (void)h;
    (void)bpc;
    (void)out;
    (void)out_stride;
    return -ENOSYS;
}

int vmaf_dnn_session_run(VmafDnnSession *sess, const VmafDnnInput *inputs, size_t n_inputs,
                         VmafDnnOutput *outputs,
                         size_t n_outputs) // NOLINT(readability-non-const-parameter)
{
    (void)sess;
    (void)inputs;
    (void)n_inputs;
    (void)outputs;
    (void)n_outputs;
    return -ENOSYS;
}

void vmaf_dnn_session_close(VmafDnnSession *sess)
{
    (void)sess;
}

const char *vmaf_dnn_session_attached_ep(VmafDnnSession *sess)
{
    (void)sess;
    return NULL;
}

#endif /* VMAF_HAVE_DNN */
