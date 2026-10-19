/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf tiny-AI functions on the VMAFx API (vmafx/dnn.h, ADR-1852 design
 * section 2.11). A VmafDnnSession is a VmafxDnnSession under its old name.
 * VmafDnnInput / VmafDnnOutput and VmafxDnnInput / VmafxDnnOutput have the
 * same members but are distinct types, so the tensor records cross as
 * copies, member by member. The library validates the arguments; its errno
 * (-ENOSYS in a build without tiny-AI support) is libvmaf's return value.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "compat_errno.h"
#include "libvmaf/dnn.h"
#include "libvmaf/libvmaf.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Up to four records of each kind are copied on the stack, more into the
 * heap, as the engine keeps its own copies (core/src/dnn/dnn_api.c). */
#define DNN_STACK_TENSORS 4u

typedef struct DnnCopies {
    VmafxDnnInput in_stack[DNN_STACK_TENSORS];
    VmafxDnnOutput out_stack[DNN_STACK_TENSORS];
    VmafxDnnInput *in;
    VmafxDnnOutput *out;
} DnnCopies;

static void dnn_copies_free(DnnCopies *c)
{
    if (c->in != c->in_stack) {
        free(c->in);
    }
    if (c->out != c->out_stack) {
        free(c->out);
    }
}

/* 0 or -ENOMEM. A NULL array stays NULL, so the library refuses it. */
static int dnn_copies_make(DnnCopies *c, const VmafDnnInput *inputs, size_t n_inputs,
                           const VmafDnnOutput *outputs, size_t n_outputs)
{
    c->in =
        (!inputs || n_inputs <= DNN_STACK_TENSORS) ? c->in_stack : calloc(n_inputs, sizeof(*c->in));
    c->out = (!outputs || n_outputs <= DNN_STACK_TENSORS) ? c->out_stack :
                                                            calloc(n_outputs, sizeof(*c->out));
    if (!c->in || !c->out) {
        dnn_copies_free(c);
        return -ENOMEM;
    }
    for (size_t i = 0; inputs && i < n_inputs; i++) {
        c->in[i].name = inputs[i].name;
        c->in[i].data = inputs[i].data;
        c->in[i].shape = inputs[i].shape;
        c->in[i].rank = inputs[i].rank;
    }
    for (size_t i = 0; outputs && i < n_outputs; i++) {
        c->out[i].name = outputs[i].name;
        c->out[i].data = outputs[i].data;
        c->out[i].capacity = outputs[i].capacity;
        c->out[i].written = outputs[i].written;
    }
    return 0;
}

static VmafxDnnConfig dnn_config(const VmafDnnConfig *cfg)
{
    VmafxDnnConfig config = VMAFX_DNN_CONFIG_INIT;
    config.device = (uint32_t)cfg->device;
    config.device_index = cfg->device_index;
    config.threads = cfg->threads;
    config.flags = cfg->fp16_io ? VMAFX_DNN_FP16_IO : 0u;
    return config;
}

static VmafxDnnSession *session_of(VmafDnnSession *sess)
{
    return (VmafxDnnSession *)sess;
}

int vmaf_use_tiny_model(VmafContext *ctx, const char *onnx_path, const VmafDnnConfig *cfg)
{
    const VmafxDnnConfig config = cfg ? dnn_config(cfg) : (VmafxDnnConfig)VMAFX_DNN_CONFIG_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_context_use_tiny_model(
        vmafx_context_from_libvmaf(ctx), onnx_path, cfg ? &config : NULL, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_dnn_set_codec_context(VmafContext *ctx, const char *codec_name, const char *preset,
                               int crf)
{
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_context_set_codec_context(vmafx_context_from_libvmaf(ctx),
                                                               codec_name, preset, crf, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_dnn_is_codec_aware(const VmafContext *ctx)
{
    return (int)vmafx_context_is_codec_aware(vmafx_context_from_libvmaf((VmafContext *)ctx));
}

int vmaf_dnn_set_resize_mode(VmafContext *ctx, VmafDnnResizeMode mode)
{
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_set_tiny_resize(vmafx_context_from_libvmaf(ctx), (uint32_t)mode, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_dnn_session_open(VmafDnnSession **out, const char *onnx_path, const VmafDnnConfig *cfg)
{
    const VmafxDnnConfig config = cfg ? dnn_config(cfg) : (VmafxDnnConfig)VMAFX_DNN_CONFIG_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_dnn_session_open(onnx_path, cfg ? &config : NULL, (VmafxDnnSession **)out, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_dnn_session_run_luma8(VmafDnnSession *sess, const uint8_t *in, size_t in_stride, int w,
                               int h, uint8_t *out, size_t out_stride)
{
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_dnn_session_run_luma8(
        session_of(sess), in, in_stride, (uint32_t)w, (uint32_t)h, out, out_stride, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_dnn_session_run_plane16(VmafDnnSession *sess, const uint16_t *in, size_t in_stride, int w,
                                 int h, int bpc, uint16_t *out, size_t out_stride)
{
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_dnn_session_run_plane16(session_of(sess), in, in_stride, (uint32_t)w, (uint32_t)h,
                                      (uint32_t)bpc, out, out_stride, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_dnn_session_run(VmafDnnSession *sess, const VmafDnnInput *inputs, size_t n_inputs,
                         VmafDnnOutput *outputs, size_t n_outputs)
{
    DnnCopies copies;
    const int err = dnn_copies_make(&copies, inputs, n_inputs, outputs, n_outputs);
    if (err) {
        return err;
    }
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_dnn_session_run(session_of(sess), inputs ? copies.in : NULL, n_inputs,
                              outputs ? copies.out : NULL, n_outputs, &error);
    for (size_t i = 0; outputs && i < n_outputs; i++) {
        outputs[i].written = copies.out[i].written;
    }
    dnn_copies_free(&copies);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

void vmaf_dnn_session_close(VmafDnnSession *sess)
{
    vmafx_dnn_session_close(session_of(sess));
}

const char *vmaf_dnn_session_attached_ep(VmafDnnSession *sess)
{
    return vmafx_dnn_session_runtime_device(session_of(sess));
}

int vmaf_dnn_verify_signature(const char *onnx_path, const char *registry_path)
{
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_dnn_verify_signature(onnx_path, registry_path, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

/* NOLINTEND(modernize-use-nullptr) */
