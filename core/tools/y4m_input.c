/*Daala video codec
Copyright (c) 2002-2007 Daala project contributors.  All rights reserved.

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

/* Vendored Daala Y4M parser. sscanf return-code checks guard against malformed
 * headers; per-check suppression would clutter the file. */
// NOLINTBEGIN(bugprone-unchecked-string-to-number-conversion,cert-err34-c) — ADR-1155: sscanf return-code checks guard against malformed headers in Daala parser

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#include "vidinput.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct y4m_input y4m_input;

/*The function used to perform chroma conversion.*/
typedef void (*y4m_convert_func)(y4m_input *y4m, unsigned char *dst, const unsigned char *aux);

/** Linkage will break without this if using a C++ compiler, and will issue
 * warnings without this for a C compiler*/
#if defined(__cplusplus)
#define OC_EXTERN extern
#else
#define OC_EXTERN
#endif

#define OC_MINI(_a, _b) ((_a) > (_b) ? (_b) : (_a))
#define OC_MAXI(_a, _b) ((_a) < (_b) ? (_b) : (_a))
#define OC_CLAMPI(_a, _b, _c) (OC_MAXI(_a, OC_MINI(_b, _c)))

struct y4m_input {
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
    char interlace;
    int src_c_dec_h;
    int src_c_dec_v;
    int dst_c_dec_h;
    int dst_c_dec_v;
    char chroma_type[16];
    int depth;
    /*The size of each converted frame buffer.*/
    size_t dst_buf_sz;
    /*The amount to read directly into the converted frame buffer.*/
    size_t dst_buf_read_sz;
    /*The size of the auxilliary buffer.*/
    size_t aux_buf_sz;
    /*The amount to read into the auxilliary buffer.*/
    size_t aux_buf_read_sz;
    y4m_convert_func convert;
    unsigned char *dst_buf;
    unsigned char *aux_buf;
};

typedef struct {
    int got_w;
    int got_h;
    int got_fps;
    int got_interlace;
    int got_par;
    int got_chroma;
} Y4MTagState;

static int y4m_process_single_tag(y4m_input *const y4m, const char *const p, const char *const q,
                                  Y4MTagState *const state)
{
    switch (p[0]) {
    case 'W':
        if (sscanf(p + 1, "%d", &y4m->pic_w) != 1)
            return -1;
        state->got_w = 1;
        break;
    case 'H':
        if (sscanf(p + 1, "%d", &y4m->pic_h) != 1)
            return -1;
        state->got_h = 1;
        break;
    case 'F':
        if (sscanf(p + 1, "%d:%d", &y4m->fps_n, &y4m->fps_d) != 2)
            return -1;
        state->got_fps = 1;
        break;
    case 'I':
        y4m->interlace = p[1];
        state->got_interlace = 1;
        break;
    case 'A':
        if (sscanf(p + 1, "%d:%d", &y4m->par_n, &y4m->par_d) != 2)
            return -1;
        state->got_par = 1;
        break;
    case 'C': {
        const ptrdiff_t tag_len = q - p - 1;
        if (tag_len > 15 || tag_len < 0)
            return -1;
        (void)memcpy(y4m->chroma_type, p + 1, (size_t)tag_len);
        y4m->chroma_type[tag_len] = '\0';
        state->got_chroma = 1;
        break;
    }
    default:
        break;
    }
    return 0;
}

static int y4m_parse_tags(y4m_input *const y4m, char *const tags)
{
    Y4MTagState state = {0};
    char *q = NULL;
    for (char *p = tags;; p = q) {
        while (*p == ' ')
            p++;
        if (p[0] == '\0')
            break;
        for (q = p + 1; *q != '\0' && *q != ' '; q++)
            ;
        if (y4m_process_single_tag(y4m, p, q, &state) < 0)
            return -1;
    }
    if (!state.got_w || !state.got_h || !state.got_fps)
        return -1;
    if (!state.got_interlace)
        y4m->interlace = '?';
    if (!state.got_par)
        y4m->par_n = y4m->par_d = 0;
    if (!state.got_chroma)
        (void)strcpy(y4m->chroma_type, "420");
    return 0;
}

static void y4m_horizontal_filter_row(unsigned char *const tmp, const unsigned char *const aux,
                                      const ptrdiff_t c_w)
{
    ptrdiff_t x = 0;
    for (x = 0; x < OC_MINI(c_w, 2); x++) {
        tmp[x] = (unsigned char)OC_CLAMPI(0,
                                          (4 * aux[0] - 17 * aux[OC_MAXI(x - 1, 0)] + 114 * aux[x] +
                                           35 * aux[OC_MINI(x + 1, c_w - 1)] -
                                           9 * aux[OC_MINI(x + 2, c_w - 1)] +
                                           aux[OC_MINI(x + 3, c_w - 1)] + 64) >>
                                              7,
                                          255);
    }
    for (; x < c_w - 3; x++) {
        tmp[x] = (unsigned char)OC_CLAMPI(0,
                                          (4 * aux[x - 2] - 17 * aux[x - 1] + 114 * aux[x] +
                                           35 * aux[x + 1] - 9 * aux[x + 2] + aux[x + 3] + 64) >>
                                              7,
                                          255);
    }
    for (; x < c_w; x++) {
        tmp[x] = (unsigned char)OC_CLAMPI(0,
                                          (4 * aux[x - 2] - 17 * aux[x - 1] + 114 * aux[x] +
                                           35 * aux[OC_MINI(x + 1, c_w - 1)] -
                                           9 * aux[OC_MINI(x + 2, c_w - 1)] + aux[c_w - 1] + 64) >>
                                              7,
                                          255);
    }
}

static void y4m_convert_42xmpeg2_42xjpeg(y4m_input *const y4m, unsigned char *dst,
                                         const unsigned char *aux)
{
    dst += (size_t)y4m->pic_w * y4m->pic_h;
    const ptrdiff_t c_w = (y4m->pic_w + y4m->dst_c_dec_h - 1) / y4m->dst_c_dec_h;
    const ptrdiff_t c_h = (y4m->pic_h + y4m->dst_c_dec_v - 1) / y4m->dst_c_dec_v;
    for (int pli = 1; pli < 3; pli++) {
        for (ptrdiff_t y = 0; y < c_h; y++) {
            y4m_horizontal_filter_row(dst, aux, c_w);
            dst += c_w;
            aux += c_w;
        }
    }
}

static void y4m_vertical_filter_cb(unsigned char *dst, const unsigned char *tmp,
                                   const ptrdiff_t c_w, const ptrdiff_t c_h, const ptrdiff_t c_sz)
{
    tmp -= c_sz;
    for (ptrdiff_t x = 0; x < c_w; x++) {
        ptrdiff_t y = 0;
        for (y = 0; y < OC_MINI(c_h, 3); y++) {
            dst[y * c_w] = (unsigned char)OC_CLAMPI(
                0,
                (tmp[0] - 9 * tmp[OC_MAXI(y - 2, 0) * c_w] + 35 * tmp[OC_MAXI(y - 1, 0) * c_w] +
                 114 * tmp[y * c_w] - 17 * tmp[OC_MINI(y + 1, c_h - 1) * c_w] +
                 4 * tmp[OC_MINI(y + 2, c_h - 1) * c_w] + 64) >>
                    7,
                255);
        }
        for (; y < c_h - 2; y++) {
            dst[y * c_w] = (unsigned char)OC_CLAMPI(
                0,
                (tmp[(y - 3) * c_w] - 9 * tmp[(y - 2) * c_w] + 35 * tmp[(y - 1) * c_w] +
                 114 * tmp[y * c_w] - 17 * tmp[(y + 1) * c_w] + 4 * tmp[(y + 2) * c_w] + 64) >>
                    7,
                255);
        }
        for (; y < c_h; y++) {
            dst[y * c_w] = (unsigned char)OC_CLAMPI(0,
                                                    (tmp[(y - 3) * c_w] - 9 * tmp[(y - 2) * c_w] +
                                                     35 * tmp[(y - 1) * c_w] + 114 * tmp[y * c_w] -
                                                     17 * tmp[OC_MINI(y + 1, c_h - 1) * c_w] +
                                                     4 * tmp[(c_h - 1) * c_w] + 64) >>
                                                        7,
                                                    255);
        }
        dst++;
        tmp++;
    }
}

static void y4m_vertical_filter_cr(unsigned char *dst, const unsigned char *tmp,
                                   const ptrdiff_t c_w, const ptrdiff_t c_h, const ptrdiff_t c_sz)
{
    tmp -= c_sz;
    for (ptrdiff_t x = 0; x < c_w; x++) {
        ptrdiff_t y = 0;
        for (y = 0; y < OC_MINI(c_h, 2); y++) {
            dst[y * c_w] = (unsigned char)OC_CLAMPI(
                0,
                (4 * tmp[0] - 17 * tmp[OC_MAXI(y - 1, 0) * c_w] + 114 * tmp[y * c_w] +
                 35 * tmp[OC_MINI(y + 1, c_h - 1) * c_w] - 9 * tmp[OC_MINI(y + 2, c_h - 1) * c_w] +
                 tmp[OC_MINI(y + 3, c_h - 1) * c_w] + 64) >>
                    7,
                255);
        }
        for (; y < c_h - 3; y++) {
            dst[y * c_w] = (unsigned char)OC_CLAMPI(
                0,
                (4 * tmp[(y - 2) * c_w] - 17 * tmp[(y - 1) * c_w] + 114 * tmp[y * c_w] +
                 35 * tmp[(y + 1) * c_w] - 9 * tmp[(y + 2) * c_w] + tmp[(y + 3) * c_w] + 64) >>
                    7,
                255);
        }
        for (; y < c_h; y++) {
            dst[y * c_w] = (unsigned char)OC_CLAMPI(
                0,
                (4 * tmp[(y - 2) * c_w] - 17 * tmp[(y - 1) * c_w] + 114 * tmp[y * c_w] +
                 35 * tmp[OC_MINI(y + 1, c_h - 1) * c_w] - 9 * tmp[OC_MINI(y + 2, c_h - 1) * c_w] +
                 tmp[(c_h - 1) * c_w] + 64) >>
                    7,
                255);
        }
        dst++;
        tmp++;
    }
}

static void y4m_convert_42xpaldv_42xjpeg(y4m_input *const y4m, unsigned char *dst,
                                         const unsigned char *aux)
{
    dst += (size_t)y4m->pic_w * y4m->pic_h;
    const ptrdiff_t c_w = (y4m->pic_w + 1) / 2;
    const ptrdiff_t c_h = (y4m->pic_h + y4m->dst_c_dec_h - 1) / y4m->dst_c_dec_h;
    const ptrdiff_t c_sz = c_w * c_h;
    unsigned char *tmp = (unsigned char *)aux + 2U * (size_t)c_sz;
    for (int pli = 1; pli < 3; pli++) {
        for (ptrdiff_t y = 0; y < c_h; y++) {
            y4m_horizontal_filter_row(tmp, aux, c_w);
            tmp += c_w;
            aux += c_w;
        }
        switch (pli) {
        case 1:
            y4m_vertical_filter_cb(dst, tmp, c_w, c_h, c_sz);
            dst += c_sz;
            break;
        case 2:
            y4m_vertical_filter_cr(dst, tmp, c_w, c_h, c_sz);
            break;
        default:
            break;
        }
    }
}

static void y4m_filter_411_row(unsigned char *const dst, const unsigned char *const aux,
                               const int c_w, const int dst_c_w)
{
    int x = 0;
    for (x = 0; x < OC_MINI(c_w, 1); x++) {
        dst[x << 1] = (unsigned char)OC_CLAMPI(
            0, (111 * aux[0] + 18 * aux[OC_MINI(1, c_w - 1)] - aux[OC_MINI(2, c_w - 1)] + 64) >> 7,
            255);
        if ((x << 1 | 1) < dst_c_w) {
            dst[x << 1 | 1] = (unsigned char)OC_CLAMPI(
                0,
                (47 * aux[0] + 86 * aux[OC_MINI(1, c_w - 1)] - 5 * aux[OC_MINI(2, c_w - 1)] + 64) >>
                    7,
                255);
        }
    }
    for (; x < c_w - 2; x++) {
        dst[x << 1] = (unsigned char)OC_CLAMPI(
            0, (aux[x - 1] + 110 * aux[x] + 18 * aux[x + 1] - aux[x + 2] + 64) >> 7, 255);
        if ((x << 1 | 1) < dst_c_w) {
            dst[x << 1 | 1] = (unsigned char)OC_CLAMPI(
                0, (-3 * aux[x - 1] + 50 * aux[x] + 86 * aux[x + 1] - 5 * aux[x + 2] + 64) >> 7,
                255);
        }
    }
    for (; x < c_w; x++) {
        dst[x << 1] = (unsigned char)OC_CLAMPI(
            0,
            (aux[x - 1] + 110 * aux[x] + 18 * aux[OC_MINI(x + 1, c_w - 1)] - aux[c_w - 1] + 64) >>
                7,
            255);
        if ((x << 1 | 1) < dst_c_w) {
            dst[x << 1 | 1] = (unsigned char)OC_CLAMPI(0,
                                                       (-3 * aux[x - 1] + 50 * aux[x] +
                                                        86 * aux[OC_MINI(x + 1, c_w - 1)] -
                                                        5 * aux[c_w - 1] + 64) >>
                                                           7,
                                                       255);
        }
    }
}

static void y4m_convert_411_422jpeg(y4m_input *const y4m, unsigned char *dst,
                                    const unsigned char *aux)
{
    dst += (size_t)y4m->pic_w * y4m->pic_h;
    const int c_w = (y4m->pic_w + y4m->src_c_dec_h - 1) / y4m->src_c_dec_h;
    const int dst_c_w = (y4m->pic_w + y4m->dst_c_dec_h - 1) / y4m->dst_c_dec_h;
    const int c_h = (y4m->pic_h + y4m->dst_c_dec_v - 1) / y4m->dst_c_dec_v;
    for (int pli = 1; pli < 3; pli++) {
        for (int y = 0; y < c_h; y++) {
            y4m_filter_411_row(dst, aux, c_w, dst_c_w);
            dst += dst_c_w;
            aux += c_w;
        }
    }
}

static void y4m_convert_mono_420jpeg(y4m_input *const y4m, unsigned char *dst,
                                     const unsigned char *const aux)
{
    (void)aux;
    dst += (size_t)y4m->pic_w * y4m->pic_h;
    const size_t c_sz = (size_t)((y4m->pic_w + y4m->dst_c_dec_h - 1) / y4m->dst_c_dec_h) *
                        (size_t)((y4m->pic_h + y4m->dst_c_dec_v - 1) / y4m->dst_c_dec_v);
    (void)memset(dst, 128, c_sz * 2U);
}

// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-1155: conforms to y4m_convert_func signature
static void y4m_convert_null(y4m_input *const y4m, unsigned char *const dst,
                             const unsigned char *const aux)
{
    (void)y4m;
    (void)dst;
    (void)aux;
}

#define Y4M_HEADER_BUFSIZE 256
#define Y4M_MAX_FRAME_PIXELS ((size_t)8192u * 8192u)

static int y4m_setup_chroma_format_part1(y4m_input *const y4m)
{
    if (strcmp(y4m->chroma_type, "420") == 0 || strcmp(y4m->chroma_type, "420jpeg") == 0 ||
        strcmp(y4m->chroma_type, "420mpeg2") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 2;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h +
                               (size_t)2 * ((y4m->pic_w + 1) / 2) * ((y4m->pic_h + 1) / 2);
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "420p10") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 2;
        y4m->dst_buf_read_sz = ((size_t)y4m->pic_w * y4m->pic_h +
                                (size_t)2 * ((y4m->pic_w + 1) / 2) * ((y4m->pic_h + 1) / 2)) *
                               2;
        y4m->depth = 10;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "420p12") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 2;
        y4m->dst_buf_read_sz = ((size_t)y4m->pic_w * y4m->pic_h +
                                (size_t)2 * ((y4m->pic_w + 1) / 2) * ((y4m->pic_h + 1) / 2)) *
                               2;
        y4m->depth = 12;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "422p10") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = 2;
        y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->depth = 10;
        y4m->dst_buf_read_sz = (size_t)2 * ((size_t)y4m->pic_w * y4m->pic_h +
                                            (size_t)2 * ((y4m->pic_w + 1) / 2) * y4m->pic_h);
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "422p12") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = 2;
        y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->depth = 12;
        y4m->dst_buf_read_sz = (size_t)2 * ((size_t)y4m->pic_w * y4m->pic_h +
                                            (size_t)2 * ((y4m->pic_w + 1) / 2) * y4m->pic_h);
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    return 1;
}

static int y4m_setup_chroma_format_part2(y4m_input *const y4m)
{
    if (strcmp(y4m->chroma_type, "444p10") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h * 3 * 2;
        y4m->depth = 10;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "444p12") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h * 3 * 2;
        y4m->depth = 12;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "420paldv") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 2;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h;
        y4m->aux_buf_sz = (size_t)3 * ((y4m->pic_w + 1) / 2) * ((y4m->pic_h + 1) / 2);
        y4m->aux_buf_read_sz = (size_t)2 * ((y4m->pic_w + 1) / 2) * ((y4m->pic_h + 1) / 2);
        y4m->convert = y4m_convert_42xpaldv_42xjpeg;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "422") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = 2;
        y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = (size_t)2 * ((y4m->pic_w + 1) / 2) * y4m->pic_h;
        y4m->convert = y4m_convert_42xmpeg2_42xjpeg;
        return 0;
    }
    return 1;
}

static int y4m_setup_chroma_format_part3(y4m_input *const y4m)
{
    if (strcmp(y4m->chroma_type, "411") == 0) {
        y4m->src_c_dec_h = 4;
        y4m->dst_c_dec_h = 2;
        y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = (size_t)2 * ((y4m->pic_w + 3) / 4) * y4m->pic_h;
        y4m->convert = y4m_convert_411_422jpeg;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "444") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h * 3;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "444alpha") == 0) {
        y4m->src_c_dec_h = y4m->dst_c_dec_h = y4m->src_c_dec_v = y4m->dst_c_dec_v = 1;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h * 3;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h;
        y4m->convert = y4m_convert_null;
        return 0;
    }
    if (strcmp(y4m->chroma_type, "mono") == 0) {
        y4m->src_c_dec_h = y4m->src_c_dec_v = 0;
        y4m->dst_c_dec_h = y4m->dst_c_dec_v = 2;
        y4m->dst_buf_read_sz = (size_t)y4m->pic_w * y4m->pic_h;
        y4m->aux_buf_sz = y4m->aux_buf_read_sz = 0;
        y4m->convert = y4m_convert_mono_420jpeg;
        return 0;
    }
    return -1;
}

static int y4m_setup_chroma_format(y4m_input *const y4m)
{
    y4m->depth = 8;
    if (y4m_setup_chroma_format_part1(y4m) == 0)
        return 0;
    if (y4m_setup_chroma_format_part2(y4m) == 0)
        return 0;
    if (y4m_setup_chroma_format_part3(y4m) == 0)
        return 0;
    (void)fprintf(stderr, "Unknown chroma sampling type: %s\n", y4m->chroma_type);
    return -1;
}

static int y4m_read_header(y4m_input *const y4m, FILE *const fin)
{
    char buffer[Y4M_HEADER_BUFSIZE];
    int i = 0;
    for (i = 0; i < Y4M_HEADER_BUFSIZE - 1; i++) {
        const size_t ret = fread(buffer + i, 1, 1, fin);
        if (ret < 1)
            return -1;
        if (buffer[i] == '\n')
            break;
    }
    buffer[i] = '\0';
    if (memcmp(buffer, "YUV4MPEG", 8) != 0) {
        (void)fprintf(stderr, "Incomplete magic for YUV4MPEG file.\n");
        return -1;
    }
    if (buffer[8] != '2') {
        (void)fprintf(stderr, "Incorrect YUV input file version; YUV4MPEG2 required.\n");
    }
    const int ret = y4m_parse_tags(y4m, buffer + 5);
    if (ret < 0) {
        (void)fprintf(stderr, "Error parsing YUV4MPEG2 header.\n");
        return ret;
    }
    return 0;
}

static int y4m_validate_dimensions(const y4m_input *const y4m)
{
    if (y4m->pic_w <= 0 || y4m->pic_h <= 0) {
        (void)fprintf(stderr, "Invalid YUV4MPEG2 dimensions: W=%d H=%d (must be > 0).\n",
                      y4m->pic_w, y4m->pic_h);
        return -1;
    }
    if ((size_t)y4m->pic_w * (size_t)y4m->pic_h > Y4M_MAX_FRAME_PIXELS) {
        (void)fprintf(stderr, "Y4M frame dimensions %dx%d exceed maximum (%dx%d).\n", y4m->pic_w,
                      y4m->pic_h, 8192, 8192);
        return -1;
    }
    if (y4m->interlace == '?') {
        (void)fprintf(stderr, "Warning: Input video interlacing format unknown; "
                              "assuming progressive scan.\n");
    } else if (y4m->interlace != 'p') {
        (void)fprintf(stderr, "Input video is interlaced; "
                              "Theora only handles progressive scan.\n");
        return -1;
    }
    return 0;
}

static int y4m_allocate_buffers(y4m_input *const y4m)
{
    const int xstride = (y4m->depth > 8) ? 2 : 1;
    y4m->dst_buf_sz = (size_t)y4m->pic_w * (size_t)y4m->pic_h +
                      (size_t)2 * (size_t)((y4m->pic_w + y4m->dst_c_dec_h - 1) / y4m->dst_c_dec_h) *
                          (size_t)((y4m->pic_h + y4m->dst_c_dec_v - 1) / y4m->dst_c_dec_v);
    y4m->dst_buf_sz *= (size_t)xstride;
    y4m->frame_w = (y4m->pic_w + 15) & ~0xF;
    y4m->frame_h = (y4m->pic_h + 15) & ~0xF;
    y4m->pic_x = ((y4m->frame_w - y4m->pic_w) >> 1) & ~1;
    y4m->pic_y = ((y4m->frame_h - y4m->pic_h) >> 1) & ~1;

    y4m->dst_buf = (unsigned char *)malloc(y4m->dst_buf_sz);
    if (!y4m->dst_buf) {
        (void)fprintf(stderr, "Could not allocate y4m destination buffer (%zu bytes).\n",
                      y4m->dst_buf_sz);
        return -1;
    }
    if (y4m->aux_buf_sz > 0) {
        y4m->aux_buf = (unsigned char *)malloc(y4m->aux_buf_sz);
        if (!y4m->aux_buf) {
            (void)fprintf(stderr, "Could not allocate y4m auxiliary buffer (%zu bytes).\n",
                          y4m->aux_buf_sz);
            free(y4m->dst_buf);
            y4m->dst_buf = NULL;
            return -1;
        }
    } else {
        y4m->aux_buf = NULL;
    }
    return 0;
}

static int y4m_input_open_impl(y4m_input *const y4m, FILE *const fin)
{
    if (y4m_read_header(y4m, fin) < 0)
        return -1;
    if (y4m_validate_dimensions(y4m) < 0)
        return -1;
    if (y4m_setup_chroma_format(y4m) < 0)
        return -1;
    return y4m_allocate_buffers(y4m);
}

static y4m_input *y4m_input_open(FILE *const fin)
{
    y4m_input *const y4m = (y4m_input *)malloc(sizeof(*y4m));
    if (!y4m) {
        (void)fprintf(stderr, "Could not allocate y4m reader state.\n");
        return NULL;
    }
    (void)memset(y4m, 0, sizeof(*y4m));
    if (y4m_input_open_impl(y4m, fin) < 0) {
        (void)fprintf(stderr, "Error opening y4m file.\n");
        free(y4m->dst_buf);
        free(y4m->aux_buf);
        free(y4m);
        return NULL;
    }
    return y4m;
}

static void y4m_input_get_info(const y4m_input *const y4m, video_input_info *const info)
{
    info->frame_w = y4m->frame_w;
    info->frame_h = y4m->frame_h;
    info->pic_w = y4m->pic_w;
    info->pic_h = y4m->pic_h;
    info->pic_x = y4m->pic_x;
    info->pic_y = y4m->pic_y;
    info->fps_n = y4m->fps_n;
    info->fps_d = y4m->fps_d;
    info->par_n = y4m->par_n;
    info->par_d = y4m->par_d;
    info->pixel_fmt = y4m->dst_c_dec_h == 2 ? (y4m->dst_c_dec_v == 2 ? PF_420 : PF_422) : PF_444;
    info->depth = y4m->depth;
}

static int y4m_read_frame_header(FILE *const fin)
{
    char frame[6];
    const size_t ret = fread(frame, 1, 6, fin);
    if (ret < 6)
        return 0;
    if (memcmp(frame, "FRAME", 5) != 0) {
        (void)fprintf(stderr, "Loss of framing in YUV input data\n");
        return -1;
    }
    if (frame[5] != '\n') {
        char c = 0;
        int j = 0;
        for (j = 0; j < 79 && fread(&c, 1, 1, fin) && c != '\n'; j++)
            ;
        if (j == 79) {
            (void)fprintf(stderr, "Error parsing YUV frame header\n");
            return -1;
        }
    }
    return 1;
}

static int y4m_input_fetch_frame(y4m_input *const y4m, FILE *const fin, video_input_ycbcr ycbcr,
                                 char tag[5])
{
    const int hdr_status = y4m_read_frame_header(fin);
    if (hdr_status <= 0)
        return hdr_status;

    const int xstride = (y4m->depth > 8) ? 2 : 1;
    const size_t pic_sz = (size_t)y4m->pic_w * (size_t)y4m->pic_h * (size_t)xstride;
    const int frame_c_w = y4m->frame_w / y4m->dst_c_dec_h;
    const int frame_c_h = y4m->frame_h / y4m->dst_c_dec_v;
    const int c_w = (y4m->pic_w + y4m->dst_c_dec_h - 1) / y4m->dst_c_dec_h;
    const int c_h = (y4m->pic_h + y4m->dst_c_dec_v - 1) / y4m->dst_c_dec_v;
    const size_t c_sz = (size_t)c_w * (size_t)c_h * (size_t)xstride;

    if (fread(y4m->dst_buf, 1, y4m->dst_buf_read_sz, fin) != y4m->dst_buf_read_sz) {
        (void)fprintf(stderr, "Error reading YUV frame data.\n");
        return -1;
    }
    if (y4m->aux_buf_read_sz > 0) {
        if (fread(y4m->aux_buf, 1, y4m->aux_buf_read_sz, fin) != y4m->aux_buf_read_sz) {
            (void)fprintf(stderr, "Error reading YUV frame data.\n");
            return -1;
        }
    }
    (*y4m->convert)(y4m, y4m->dst_buf, y4m->aux_buf);

    ycbcr[0].width = y4m->frame_w;
    ycbcr[0].height = y4m->frame_h;
    ycbcr[0].stride = y4m->pic_w * xstride;
    ycbcr[0].data = y4m->dst_buf - ((size_t)y4m->pic_x + (size_t)y4m->pic_y * y4m->pic_w) * xstride;
    ycbcr[1].width = frame_c_w;
    ycbcr[1].height = frame_c_h;
    ycbcr[1].stride = c_w * xstride;
    ycbcr[1].data =
        y4m->dst_buf + pic_sz -
        ((size_t)(y4m->pic_x / y4m->dst_c_dec_h) + (size_t)(y4m->pic_y / y4m->dst_c_dec_v) * c_w) *
            xstride;
    ycbcr[2].width = frame_c_w;
    ycbcr[2].height = frame_c_h;
    ycbcr[2].stride = c_w * xstride;
    ycbcr[2].data = ycbcr[1].data + c_sz;
    if (tag)
        tag[0] = '\0';
    return 1;
}

static void y4m_input_close(y4m_input *const y4m)
{
    free(y4m->dst_buf);
    free(y4m->aux_buf);
}

static int y4m_read_plane_rows(FILE *const fin, uint8_t *dst, const size_t row_bytes,
                               const ptrdiff_t stride, const unsigned height)
{
    for (unsigned j = 0; j < height; j++) {
        if (fread(dst, 1, row_bytes, fin) != row_bytes) {
            (void)fprintf(stderr, "Error reading YUV frame data.\n");
            return -1;
        }
        dst += stride;
    }
    return 0;
}

static int y4m_fetch_into_vmaf_picture(y4m_input *const y4m, FILE *const fin,
                                       VmafPicture *const pic)
{
    if (y4m->convert != y4m_convert_null) {
        (void)fprintf(stderr, "y4m format requires conversion; direct read not supported.\n");
        return -1;
    }

    const int hdr_status = y4m_read_frame_header(fin);
    if (hdr_status <= 0)
        return hdr_status;

    const size_t bytes_per_sample = ((size_t)pic->bpc + 7) / 8;

    for (unsigned i = 0; i < 3; i++) {
        const size_t row_bytes = (size_t)pic->w[i] * bytes_per_sample;
        if (pic->stride[i] == (ptrdiff_t)row_bytes) {
            if (fread(pic->data[i], 1, row_bytes * pic->h[i], fin) != row_bytes * pic->h[i]) {
                (void)fprintf(stderr, "Error reading YUV frame data.\n");
                return -1;
            }
        } else {
            if (y4m_read_plane_rows(fin, (uint8_t *)pic->data[i], row_bytes, pic->stride[i],
                                    pic->h[i]) < 0)
                return -1;
        }
    }

    return 1;
}

static void *y4m_vtbl_open(FILE *const fin)
{
    return y4m_input_open(fin);
}

static void y4m_vtbl_get_info(void *const ctx, video_input_info *const info)
{
    y4m_input_get_info((const y4m_input *)ctx, info);
}

static int y4m_vtbl_fetch_frame(void *const ctx, FILE *const fin, video_input_ycbcr ycbcr,
                                char tag[5])
{
    return y4m_input_fetch_frame((y4m_input *)ctx, fin, ycbcr, tag);
}

static void y4m_vtbl_close(void *const ctx)
{
    y4m_input_close((y4m_input *)ctx);
}

static int y4m_vtbl_fetch_into_vmaf_picture(void *const ctx, FILE *const fin,
                                            VmafPicture *const pic)
{
    return y4m_fetch_into_vmaf_picture((y4m_input *)ctx, fin, pic);
}

// NOLINTNEXTLINE(misc-use-internal-linkage,cppcoreguidelines-avoid-non-const-global-variables) — ADR-1155: extern linkage required by vidinput.c
OC_EXTERN const video_input_vtbl Y4M_INPUT_VTBL = {
    .open_raw = NULL,
    .open = y4m_vtbl_open,
    .get_info = y4m_vtbl_get_info,
    .fetch_frame = y4m_vtbl_fetch_frame,
    .close = y4m_vtbl_close,
    .fetch_into_vmaf_picture = y4m_vtbl_fetch_into_vmaf_picture,
};

// NOLINTEND(modernize-use-nullptr)
// NOLINTEND(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
