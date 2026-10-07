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
 * Hardware decoding for loopback decoders (-dec): the -hwaccel,
 * -hwaccel_device and -hwaccel_output_format options given before -dec,
 * read as an input stream's (ffmpeg_demux.c), so an encoder's output can be
 * decoded on the GPU and reach a filter as hardware frames. Carried by the
 * VMAFx fork only (decision Q-048); source of truth:
 * ffmpeg-patches/src/ffmpeg_dec_hwaccel.c in the VMAFx tree.
 */

#include <string.h>

#include "ffmpeg.h"

#include "libavutil/hwcontext.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

/* The last value given: a loopback decoder has one stream. */
static const char *last_str(const SpecifierOptList *list)
{
    return list->nb_opt ? list->opt[list->nb_opt - 1].u.str : NULL;
}

static int parse_hwaccel(void *logctx, const char *hwaccel, DecoderHwaccelOpts *hw)
{
    enum AVHWDeviceType type;
    /* The NVDEC hwaccels use a CUDA device, as for an input stream. */
    if (!strcmp(hwaccel, "nvdec") || !strcmp(hwaccel, "cuvid"))
        hwaccel = "cuda";
    if (!strcmp(hwaccel, "none")) {
        hw->hwaccel_id = HWACCEL_NONE;
        return 0;
    }
    if (!strcmp(hwaccel, "auto")) {
        hw->hwaccel_id = HWACCEL_AUTO;
        return 0;
    }
    type = av_hwdevice_find_type_by_name(hwaccel);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        av_log(logctx, AV_LOG_FATAL, "Unrecognized hwaccel for a loopback decoder: %s\n", hwaccel);
        return AVERROR(EINVAL);
    }
    hw->hwaccel_id = HWACCEL_GENERIC;
    hw->hwaccel_device_type = type;
    return 0;
}

int dec_hwaccel_opts_parse(void *logctx, const OptionsContext *o, DecoderHwaccelOpts *hw)
{
    const char *hwaccel = last_str(&o->hwaccels);
    const char *device = last_str(&o->hwaccel_devices);
    const char *format = last_str(&o->hwaccel_output_formats);
    int ret;

    hw->hwaccel_id = HWACCEL_NONE;
    hw->hwaccel_device_type = AV_HWDEVICE_TYPE_NONE;
    hw->hwaccel_device = NULL;
    hw->hwaccel_output_format = AV_PIX_FMT_NONE;
    if (format) {
        hw->hwaccel_output_format = av_get_pix_fmt(format);
        if (hw->hwaccel_output_format == AV_PIX_FMT_NONE) {
            av_log(logctx, AV_LOG_FATAL, "Unrecognised hwaccel output format: %s\n", format);
            return AVERROR(EINVAL);
        }
    }
    if (hwaccel && (ret = parse_hwaccel(logctx, hwaccel, hw)) < 0)
        return ret;
    if (device) {
        hw->hwaccel_device = av_strdup(device);
        if (!hw->hwaccel_device)
            return AVERROR(ENOMEM);
    }
    return 0;
}

void dec_hwaccel_opts_apply(const DecoderHwaccelOpts *hw, DecoderOpts *o)
{
    o->hwaccel_id = hw->hwaccel_id;
    o->hwaccel_device_type = hw->hwaccel_device_type;
    o->hwaccel_device = hw->hwaccel_device;
    o->hwaccel_output_format = hw->hwaccel_output_format;
}

void dec_hwaccel_opts_free(DecoderHwaccelOpts *hw)
{
    av_freep(&hw->hwaccel_device);
}
