/*Daala video codec
Copyright (c) 2002-2007 Daala project contributors.  All rights reserved.
SPDX-License-Identifier: BSD-2-Clause

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

/* NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp):
 * this header is Daala/Theora-derived (see the copyright above). The leading
 * underscore parameter names (_fin, _ctx, _ti, ...) and the _vidinput_H guard
 * are the upstream spellings; renaming them would break parity with the code
 * this was ported from and churn every caller. _LARGEFILE_SOURCE and
 * _LARGEFILE64_SOURCE are the standard feature-test macros, reserved by
 * definition. ADR-0141 §2 / ADR-0278. */
#if !defined(_vidinput_H)
#define _vidinput_H (1)
#if !defined(_LARGEFILE_SOURCE)
#define _LARGEFILE_SOURCE
#endif
#if !defined(_LARGEFILE64_SOURCE)
#define _LARGEFILE64_SOURCE
#endif
#if !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#include <stdio.h>
#include <stdint.h>
#include "libvmaf/picture.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using,performance-enum-size): one definition for C
 * and C++. vidinput.c, y4m_input.c, yuv_input.c and their tests are C and
 * vmaf.cpp is C++: `using` is not C, and an underlying type fixed for C++ only
 * would give video_input_pixel_format, a member of video_input_info, a
 * different size on each side of the language boundary. ADR-0141 / ADR-1470. */
typedef struct video_input video_input;
typedef struct video_input_vtbl video_input_vtbl;
typedef struct video_input_info video_input_info;
struct video_input_plane {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *data;
};
typedef struct video_input_plane video_input_ycbcr[3];

typedef void *(*video_input_open_func)(FILE *_fin);
typedef void (*video_input_get_info_func)(void *_ctx, video_input_info *_ti);
typedef int (*video_input_fetch_frame_func)(void *_ctx, FILE *_fin, video_input_ycbcr _ycbcr,
                                            char _tag[5]);
typedef void (*video_input_close_func)(void *_ctx);
typedef void *(*raw_input_open_func)(FILE *_fin, unsigned width, unsigned height, int pix_fmt,
                                     unsigned bitdepth);

typedef int (*video_input_fetch_into_vmaf_picture_func)(void *_ctx, FILE *_fin, VmafPicture *pic);
typedef int (*raw_input_set_rgb_func)(void *_ctx, unsigned matrix, unsigned range,
                                      unsigned transfer, unsigned out_range);

/**Pluggable method table for accessing different formats.*/
struct video_input_vtbl {
    raw_input_open_func open_raw;
    video_input_open_func open;
    video_input_get_info_func get_info;
    video_input_fetch_frame_func fetch_frame;
    video_input_close_func close;
    video_input_fetch_into_vmaf_picture_func fetch_into_vmaf_picture;
    raw_input_set_rgb_func set_rgb; /* NULL: the input has no RGB layout */
};

struct video_input {
    const video_input_vtbl *vtbl;
    void *ctx;
    FILE *fin;
};

/* `pix_fmt` is a VmafPixelFormat (1 to 4: planar 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0) or, from 16
 * up, a VmafxPixelFormat layout of the import table (NV12, P010, YUYV422, V210, RGBA ...):
 * the frame the reader makes is the planar frame of that layout (ADR-2145). */
int raw_input_open(video_input *_vid, FILE *_fin, unsigned width, unsigned height, int pix_fmt,
                   unsigned bitdepth);

/* The statement of a raw RGB layout (VmafxColorMatrix, VmafxColorRange,
 * VmafxColorTransfer values of vmafx/types.h): the matrix, the range of the
 * R'G'B' samples, their transfer characteristic and the range of the Y'CbCr
 * frame made (ADR-2146). Call before the first frame is read. 0 on success; -1
 * when the input is not a raw RGB layout or the statement names a conversion the
 * reference does not make. A raw RGB input read without it fails to open. */
int raw_input_set_rgb(video_input *_vid, unsigned matrix, unsigned range, unsigned transfer,
                      unsigned out_range);

int video_input_open(video_input *_vid, FILE *_fin);
void video_input_close(video_input *_vid);

void video_input_get_info(video_input *_vid, video_input_info *_ti);
int video_input_fetch_frame(video_input *_vid, video_input_ycbcr _ycbcr, char _tag[5]);
int video_input_fetch_into_vmaf_picture(video_input *_vid, VmafPicture *pic);

typedef enum {
    /** Chroma decimation by 2 in both the X and Y directions (4:2:0).
   *  The Cb and Cr chroma planes are half the width and half the
   *  height of the luma plane. */
    PF_420,
    /** Currently reserved. */
    PF_RSVD,
    /** Chroma decimation by 2 in the X direction (4:2:2).
   *  The Cb and Cr chroma planes are half the width of the luma plane,
   *  but full height. */
    PF_422,
    /** No chroma decimation (4:4:4).
   *  The Cb and Cr chroma planes are full width and full height. */
    PF_444,
    /** Luma only (raw .yuv files with --pixel_format 400): no chroma planes. */
    PF_400,
    /** The total number of currently defined pixel formats. */
    PF_NFORMATS
} video_input_pixel_format;

struct video_input_info {
    int frame_w;
    int frame_h;
    int pic_w;
    int pic_h;
    int pic_x;
    int pic_y;
    int fps_n;
    int fps_d;
    int par_n;
    int par_d;
    video_input_pixel_format pixel_fmt;
    char interlace;
    char chroma_type[16];
    int depth;
};

/* The two concrete input backends. Declared here rather than with a bare
 * `extern` inside vidinput.c so both the definition and the use see one
 * declaration: without it clang-tidy sees a definition with no prior
 * declaration and proposes internal linkage (misc-use-internal-linkage), which
 * would break the link. The local externs also spelled the type WITHOUT const
 * while both definitions are const -- a type mismatch on the same symbol. */
extern const video_input_vtbl Y4M_INPUT_VTBL;
extern const video_input_vtbl YUV_INPUT_VTBL;
/* NOLINTEND(modernize-use-using,performance-enum-size) */

#if defined(__cplusplus)
} // extern "C"
#endif

#endif

/* NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp) */
