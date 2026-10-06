/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Tiny-AI models of the VMAFx API (vmafx/dnn.h, RC4 WP6): the engine's ONNX
 * Runtime surface (core/src/dnn/) under the new names. A VmafxDnnSession is
 * the engine's session object. The engine validates the arguments and
 * decides what the build supports: without tiny-AI support every call fails
 * with the engine's -ENOSYS (VMAFX_E_NOTSUP), whatever its arguments, as the
 * libvmaf functions did.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "error_internal.h"
#include "internal.h"
#include "libvmaf/dnn.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* VmafxDnnInput / VmafxDnnOutput have the members of the engine's
 * VmafDnnInput / VmafDnnOutput but are distinct types, so the records cross
 * as copies, member by member: up to four of each kind on the stack, more in
 * the heap, as the engine keeps its own copies (core/src/dnn/dnn_api.c). */
#define DNN_STACK_TENSORS 4u

typedef struct EngineTensors {
    VmafDnnInput in_stack[DNN_STACK_TENSORS];
    VmafDnnOutput out_stack[DNN_STACK_TENSORS];
    VmafDnnInput *in;
    VmafDnnOutput *out;
} EngineTensors;

static void engine_tensors_free(EngineTensors *t)
{
    if (t->in != t->in_stack) {
        free(t->in);
    }
    if (t->out != t->out_stack) {
        free(t->out);
    }
}

/* 0 or -ENOMEM. A NULL array stays NULL, so the engine refuses it. */
static int engine_tensors_make(EngineTensors *t, const VmafxDnnInput *inputs, size_t n_inputs,
                               const VmafxDnnOutput *outputs, size_t n_outputs)
{
    t->in =
        (!inputs || n_inputs <= DNN_STACK_TENSORS) ? t->in_stack : calloc(n_inputs, sizeof(*t->in));
    t->out = (!outputs || n_outputs <= DNN_STACK_TENSORS) ? t->out_stack :
                                                            calloc(n_outputs, sizeof(*t->out));
    if (!t->in || !t->out) {
        engine_tensors_free(t);
        return -ENOMEM;
    }
    for (size_t i = 0; inputs && i < n_inputs; i++) {
        t->in[i].name = inputs[i].name;
        t->in[i].data = inputs[i].data;
        t->in[i].shape = inputs[i].shape;
        t->in[i].rank = inputs[i].rank;
    }
    for (size_t i = 0; outputs && i < n_outputs; i++) {
        t->out[i].name = outputs[i].name;
        t->out[i].data = outputs[i].data;
        t->out[i].capacity = outputs[i].capacity;
        t->out[i].written = outputs[i].written;
    }
    return 0;
}

static VmafDnnSession *engine_session(VmafxDnnSession *session)
{
    return (VmafDnnSession *)session;
}

static VmafContext *engine_context(VmafxContext *context)
{
    return context ? vmafx_context_engine(context) : NULL;
}

/* The engine's failure `err` naming `subject`; -ENOSYS (a build without
 * tiny-AI support) is VMAFX_E_NOTSUP. */
static VmafxStatus dnn_failure(const VmafxReport *report, int err, const char *subject,
                               const char *what)
{
    const VmafxStatus status = err == -ENOSYS ? VMAFX_E_NOTSUP : vmafx_status_from_errno(err);
    return VMAFX_FAIL(report, status, err, VMAFX_SUBJECT_MODEL, subject, "%s failed (%d)%s", what,
                      err, err == -ENOSYS ? ": this build has no tiny-AI support" : "");
}

/* The engine configuration of `config` (NULL: the engine's defaults, NULL). */
static VmafxStatus engine_config(const VmafxReport *report, const VmafxDnnConfig *config,
                                 VmafDnnConfig *cfg, const VmafDnnConfig **out)
{
    *out = NULL;
    if (!config) {
        return VMAFX_OK;
    }
    VmafxDnnConfig c = VMAFX_DNN_CONFIG_INIT;
    const VmafxStatus status =
        vmafx_read_sized(report, &c, (uint32_t)sizeof(c), config, VMAFX_MIN_DNN_CONFIG, "config");
    if (status != VMAFX_OK) {
        return status;
    }
    cfg->device = (VmafDnnDevice)c.device;
    cfg->device_index = c.device_index;
    cfg->threads = c.threads;
    cfg->fp16_io = (c.flags & VMAFX_DNN_FP16_IO) != 0u;
    *out = cfg;
    return VMAFX_OK;
}

uint32_t vmafx_dnn_available(void)
{
    return vmaf_engine_dnn_available() ? 1u : 0u;
}

VmafxStatus vmafx_context_use_tiny_model(VmafxContext *context, const char *onnx_path,
                                         const VmafxDnnConfig *config, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    VmafDnnConfig cfg = {0};
    const VmafDnnConfig *engine_cfg = NULL;
    const VmafxStatus status = engine_config(&report, config, &cfg, &engine_cfg);
    if (status != VMAFX_OK) {
        return status;
    }
    const VmafLogSink *const previous = context ? vmafx_engine_enter(context) : NULL;
    const int err = vmaf_engine_use_tiny_model(engine_context(context), onnx_path, engine_cfg);
    if (context) {
        vmafx_engine_leave(previous);
    }
    if (err) {
        return dnn_failure(&report, err, onnx_path ? onnx_path : "onnx_path",
                           "attaching the tiny model");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_context_set_codec_context(VmafxContext *context, const char *codec,
                                            const char *preset, int32_t crf, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    const int err = vmaf_engine_dnn_set_codec_context(engine_context(context), codec, preset, crf);
    if (err) {
        return dnn_failure(&report, err, "context", "setting the codec context");
    }
    return VMAFX_OK;
}

uint32_t vmafx_context_is_codec_aware(const VmafxContext *context)
{
    const VmafContext *const engine = context ? vmafx_context_engine(context) : NULL;
    return vmaf_engine_dnn_is_codec_aware(engine) > 0 ? 1u : 0u;
}

VmafxStatus vmafx_context_set_tiny_resize(VmafxContext *context, uint32_t mode, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    const int err =
        vmaf_engine_dnn_set_resize_mode(engine_context(context), (VmafDnnResizeMode)mode);
    if (err) {
        return dnn_failure(&report, err, "mode", "setting the tiny model's resize mode");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_dnn_session_open(const char *onnx_path, const VmafxDnnConfig *config,
                                   VmafxDnnSession **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafDnnConfig cfg = {0};
    const VmafDnnConfig *engine_cfg = NULL;
    const VmafxStatus status = engine_config(&report, config, &cfg, &engine_cfg);
    if (status != VMAFX_OK) {
        return status;
    }
    const int err = vmaf_engine_dnn_session_open((VmafDnnSession **)out, onnx_path, engine_cfg);
    if (err) {
        return dnn_failure(&report, err, onnx_path ? onnx_path : "onnx_path",
                           "opening the ONNX session");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_dnn_session_run_luma8(VmafxDnnSession *session, const void *in, size_t in_stride,
                                        uint32_t w, uint32_t h, void *out, size_t out_stride,
                                        VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    const int err = vmaf_engine_dnn_session_run_luma8(engine_session(session), in, in_stride,
                                                      (int)w, (int)h, out, out_stride);
    if (err) {
        return dnn_failure(&report, err, "session", "running the session on an 8-bit plane");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_dnn_session_run_plane16(VmafxDnnSession *session, const void *in,
                                          size_t in_stride, uint32_t w, uint32_t h, uint32_t bpc,
                                          void *out, size_t out_stride, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    const int err = vmaf_engine_dnn_session_run_plane16(engine_session(session), in, in_stride,
                                                        (int)w, (int)h, (int)bpc, out, out_stride);
    if (err) {
        return dnn_failure(&report, err, "session", "running the session on a 16-bit plane");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_dnn_session_run(VmafxDnnSession *session, const VmafxDnnInput *inputs,
                                  size_t n_inputs, VmafxDnnOutput *outputs, size_t n_outputs,
                                  VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    EngineTensors tensors;
    int err = engine_tensors_make(&tensors, inputs, n_inputs, outputs, n_outputs);
    if (!err) {
        err = vmaf_engine_dnn_session_run(engine_session(session), inputs ? tensors.in : NULL,
                                          n_inputs, outputs ? tensors.out : NULL, n_outputs);
        for (size_t i = 0; outputs && i < n_outputs; i++) {
            outputs[i].written = tensors.out[i].written;
        }
        engine_tensors_free(&tensors);
    }
    if (err) {
        return dnn_failure(&report, err, "session", "running the session");
    }
    return VMAFX_OK;
}

void vmafx_dnn_session_close(VmafxDnnSession *session)
{
    vmaf_engine_dnn_session_close(engine_session(session));
}

const char *vmafx_dnn_session_runtime_device(VmafxDnnSession *session)
{
    return vmaf_engine_dnn_session_attached_ep(engine_session(session));
}

VmafxStatus vmafx_dnn_verify_signature(const char *onnx_path, const char *registry_path,
                                       VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    const int err = vmaf_engine_dnn_verify_signature(onnx_path, registry_path);
    if (err) {
        return dnn_failure(&report, err, onnx_path ? onnx_path : "onnx_path",
                           "verifying the model's signature");
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
