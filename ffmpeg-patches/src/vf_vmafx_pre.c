/*
 * Copyright 2026 Lusoris
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/**
 * @file
 * The vmafx_pre filter: a learned ONNX pre-filter run by the VMAFx library
 * (vmafx_dnn_session_*), the vmaf_pre filter under its VMAFx name with the
 * same options. 8-bit planes go through vmafx_dnn_session_run_luma8(),
 * 10- and 12-bit planes through vmafx_dnn_session_run_plane16(); with
 * chroma=1 the session also runs on U and V at their own size, otherwise
 * they are copied. A failed inference fails the frame, naming the plane.
 * Source of truth: ffmpeg-patches/src/vf_vmafx_pre.c in the VMAFx tree.
 */

#include <vmafx/vmafx.h>

#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

typedef struct VmafxPreContext {
    const AVClass *class;
    char *model_path;
    char *device;
    int threads;
    int chroma; /* apply the filter to U/V as well as Y */
    VmafxDnnSession *session;
} VmafxPreContext;

#define OFFSET(x) offsetof(VmafxPreContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption vmafx_pre_options[] = {
    {"model",
     "Path to ONNX learned-filter model.",
     OFFSET(model_path),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"device",
     "Inference device: auto|cpu|cuda|openvino|openvino-npu|openvino-cpu|openvino-gpu|coreml|"
     "coreml-ane|coreml-gpu|coreml-cpu|rocm.",
     OFFSET(device),
     AV_OPT_TYPE_STRING,
     {.str = "auto"},
     0,
     0,
     FLAGS},
    {"threads",
     "CPU EP intra-op threads.",
     OFFSET(threads),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     INT_MAX,
     FLAGS},
    {"chroma",
     "Also filter U/V planes (0/1; default 0 = luma-only).",
     OFFSET(chroma),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {NULL},
};

AVFILTER_DEFINE_CLASS(vmafx_pre);

static int parse_device(const char *name, uint32_t *out)
{
    static const struct {
        const char *name;
        uint32_t device;
    } devices[] = {
        {"auto", VMAFX_DNN_DEVICE_AUTO},
        {"cpu", VMAFX_DNN_DEVICE_CPU},
        {"cuda", VMAFX_DNN_DEVICE_CUDA},
        {"openvino", VMAFX_DNN_DEVICE_OPENVINO},
        {"openvino-npu", VMAFX_DNN_DEVICE_OPENVINO_NPU},
        {"openvino-cpu", VMAFX_DNN_DEVICE_OPENVINO_CPU},
        {"openvino-gpu", VMAFX_DNN_DEVICE_OPENVINO_GPU},
        {"coreml", VMAFX_DNN_DEVICE_COREML},
        {"coreml-ane", VMAFX_DNN_DEVICE_COREML_ANE},
        {"coreml-gpu", VMAFX_DNN_DEVICE_COREML_GPU},
        {"coreml-cpu", VMAFX_DNN_DEVICE_COREML_CPU},
        {"rocm", VMAFX_DNN_DEVICE_ROCM},
    };
    if (!name || !*name) {
        *out = VMAFX_DNN_DEVICE_AUTO;
        return 0;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(devices); i++) {
        if (!av_strcasecmp(name, devices[i].name)) {
            *out = devices[i].device;
            return 0;
        }
    }
    return AVERROR(EINVAL);
}

static av_cold int init(AVFilterContext *ctx)
{
    VmafxPreContext *s = ctx->priv;
    VmafxDnnConfig cfg = VMAFX_DNN_CONFIG_INIT;
    if (!s->model_path || !*s->model_path) {
        av_log(ctx, AV_LOG_ERROR, "vmafx_pre: model= option is required.\n");
        return AVERROR(EINVAL);
    }
    if (!vmafx_dnn_available()) {
        av_log(ctx, AV_LOG_ERROR, "vmafx_pre: libvmafx was built without tiny-AI support.\n");
        return AVERROR(ENOSYS);
    }
    if (parse_device(s->device, &cfg.device) < 0) {
        av_log(ctx, AV_LOG_ERROR, "vmafx_pre: unknown device=%s.\n", s->device ? s->device : "");
        return AVERROR(EINVAL);
    }
    cfg.threads = s->threads;
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_dnn_session_open(s->model_path, &cfg, &s->session, &error);
    if (st != VMAFX_OK) {
        av_log(ctx, AV_LOG_ERROR, "vmafx_pre: %s: %s: %s\n", s->model_path, vmafx_status_name(st),
               vmafx_error_message(error));
        vmafx_error_free(error);
        return AVERROR(EINVAL);
    }
    av_log(ctx, AV_LOG_INFO, "vmafx_pre: loaded %s (device=%s threads=%d chroma=%d)\n",
           s->model_path, s->device ? s->device : "auto", s->threads, s->chroma);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    VmafxPreContext *s = ctx->priv;
    vmafx_dnn_session_close(s->session);
    s->session = NULL;
}

/* One plane through the session: run_luma8 at 8 bits, run_plane16 above. */
static int run_plane(AVFilterContext *ctx, int bpc, const AVFrame *in, AVFrame *out, int plane,
                     int w, int h)
{
    VmafxPreContext *s = ctx->priv;
    VmafxError *error = NULL;
    VmafxStatus st;
    if (bpc == 8)
        st = vmafx_dnn_session_run_luma8(s->session, in->data[plane], (size_t)in->linesize[plane],
                                         (uint32_t)w, (uint32_t)h, out->data[plane],
                                         (size_t)out->linesize[plane], &error);
    else
        st = vmafx_dnn_session_run_plane16(s->session, in->data[plane], (size_t)in->linesize[plane],
                                           (uint32_t)w, (uint32_t)h, (uint32_t)bpc,
                                           out->data[plane], (size_t)out->linesize[plane], &error);
    if (st == VMAFX_OK)
        return 0;
    av_log(ctx, AV_LOG_ERROR, "vmafx_pre: inference of plane %d (%dx%d, %d-bit) failed: %s: %s\n",
           plane, w, h, bpc, vmafx_status_name(st), vmafx_error_message(error));
    vmafx_error_free(error);
    return AVERROR(EIO);
}

/* U and V: filtered with chroma=1, copied otherwise. */
static int chroma_planes(AVFilterContext *ctx, const AVPixFmtDescriptor *d, int bpc,
                         const AVFrame *in, AVFrame *out)
{
    VmafxPreContext *s = ctx->priv;
    const int cw = AV_CEIL_RSHIFT(in->width, d->log2_chroma_w);
    const int ch = AV_CEIL_RSHIFT(in->height, d->log2_chroma_h);
    for (int plane = 1; plane < 3; plane++) {
        if (s->chroma) {
            const int err = run_plane(ctx, bpc, in, out, plane, cw, ch);
            if (err < 0)
                return err;
        } else {
            av_image_copy_plane(out->data[plane], out->linesize[plane], in->data[plane],
                                in->linesize[plane], cw * (bpc > 8 ? 2 : 1), ch);
        }
    }
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(in->format);
    if (!d) {
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }
    const int bpc = d->comp[0].depth; /* 8, 10, 12 for the formats we admit */
    AVFrame *out = ff_get_video_buffer(outlink, in->width, in->height);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    int err = av_frame_copy_props(out, in);
    if (err >= 0)
        err = run_plane(ctx, bpc, in, out, 0, in->width, in->height);
    if (err >= 0 && in->data[1] && out->data[1])
        err = chroma_planes(ctx, d, bpc, in, out);
    av_frame_free(&in);
    if (err < 0) {
        av_frame_free(&out);
        return err;
    }
    return ff_filter_frame(outlink, out);
}

static int query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_GRAY8,    AV_PIX_FMT_YUV420P,     AV_PIX_FMT_YUV422P,     AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_GRAY10LE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV444P10LE,
        AV_PIX_FMT_GRAY12LE, AV_PIX_FMT_YUV420P12LE, AV_PIX_FMT_YUV422P12LE, AV_PIX_FMT_YUV444P12LE,
        AV_PIX_FMT_NONE,
    };
    return ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out, pix_fmts);
}

static const AVFilterPad vmafx_pre_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_vmafx_pre = {
    .p.name = "vmafx_pre",
    .p.description =
        NULL_IF_CONFIG_SMALL("Learned ONNX pre-processing filter run by the VMAFx library."),
    .p.priv_class = &vmafx_pre_class,
    .priv_size = sizeof(VmafxPreContext),
    .init = init,
    .uninit = uninit,
    FILTER_INPUTS(vmafx_pre_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
};
