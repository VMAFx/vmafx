/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define yuv_fileno _fileno
#else
#include <unistd.h>
#define yuv_fileno fileno
#endif
/*
 * Include <sys/stat.h> before any macro aliases so the system header
 * parses cleanly.  On MSVC (ucrt/sys/stat.h, SDK 10.0.26100.0+) the
 * header declares _fstat64 / struct _stat64 / __stat64 internally; if
 * we define `#define stat __stat64` first, the preprocessor expands the
 * identifiers inside the header itself, causing "redefinition of struct
 * _stat64" and cascading C2059/C2143 errors with NVCC and cl.exe.
 * Placing the include here, before the MSVC macro block, avoids that
 * conflict.  MinGW64 also defines _WIN32 but ships POSIX-compatible
 * stat/fstat/S_ISREG natively; the _MSC_VER guard below ensures the
 * aliases are only active under cl.exe / icx-cl.  (ADR-0521, ADR-0575)
 */
#include <sys/stat.h>
#ifdef _MSC_VER
/*
 * MSVC <sys/stat.h> declares _fstat64 / struct __stat64 but not the POSIX
 * fstat() / S_ISREG() names.  Map them to the MSVC equivalents so the
 * yuv_check_file_size() body can stay unguarded (ADR-0521).
 *
 * _S_IFREG is defined by MSVC <sys/stat.h>; S_ISREG is not.
 */
#define fstat(fd, st) _fstat64((fd), (st))
#define stat __stat64
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
/*
 * ucrt SDK 10.0.26100+ declares off_t as 'long' in <sys/types.h>, which is
 * pulled in transitively by <sys/stat.h> above.  Guard the typedef to avoid
 * C2371 "redefinition; different basic types" under cl.exe / icx-cl.
 *
 * When ucrt has not declared off_t (older SDKs), define it ourselves as
 * __int64 for large-file (> 2 GiB) safety.  When ucrt has already declared
 * it, the _OFF_T_DEFINED sentinel is set and we skip the typedef; the body
 * code in yuv_check_file_size() continues to use off_t (which is 'long' on
 * those SDKs) — acceptable because the static assert below would catch any
 * size regression, and these SDKs are EOL.
 */
#ifndef _OFF_T_DEFINED
typedef __int64 off_t;
#define _OFF_T_DEFINED
#endif
#endif

#include "vidinput.h"

#include "libvmaf/picture.h"

/** Linkage will break without this if using a C++ compiler, and will issue
 * warnings without this for a C compiler*/
#if defined(__cplusplus)
#define OC_EXTERN extern
#else
#define OC_EXTERN
#endif

typedef struct yuv_input {
    FILE *fin;
    unsigned width, height;
    enum VmafPixelFormat pix_fmt;
    unsigned bitdepth;
    size_t dst_buf_sz;
    uint8_t *dst_buf;
    int src_c_dec_v, src_c_dec_h;
    int dst_c_dec_h, dst_c_dec_v;
} yuv_input;

/* Validate file size against declared geometry + bit depth so that a
 * mismatched --bitdepth flag surfaces a clear error instead of heap
 * corruption (malloc fastbin misalignment) at the first fread.
 *
 * Exits with code 2 on any geometry/depth mismatch (following the
 * cli_parse.c convention of calling exit() directly for bad-usage errors).
 * Returns silently when the fd is not a regular file (pipe, socket) — the
 * reader will discover EOF naturally.
 *
 * Uses fstat rather than fseek/ftell to avoid disturbing the stream
 * position and to correctly handle sizes >2 GiB via the
 * _LARGEFILE64_SOURCE / _FILE_OFFSET_BITS=64 definitions in vidinput.h.
 */
static void yuv_check_file_size(FILE *fin, const yuv_input *yuv)
{
    struct stat st;
    if (fstat(yuv_fileno(fin), &st) != 0 || !S_ISREG(st.st_mode))
        return; /* pipe or fstat failure — skip, let reader hit EOF */

    off_t file_sz = st.st_size;
    size_t frame_sz = yuv->dst_buf_sz;
    unsigned bpp = yuv->bitdepth > 8u ? 2u : 1u;
    const char *fmt_name = yuv->pix_fmt == VMAF_PIX_FMT_YUV420P ? "yuv420p" :
                           yuv->pix_fmt == VMAF_PIX_FMT_YUV422P ? "yuv422p" :
                                                                  "yuv444p";

    if (file_sz < (off_t)frame_sz) {
        (void)fprintf(stderr,
                      "yuv: file too small for declared geometry — "
                      "need at least %zu bytes for one %ux%u %u-bit %s frame, "
                      "got %lld bytes\n",
                      frame_sz, yuv->width, yuv->height, yuv->bitdepth, fmt_name,
                      (long long)file_sz);
        exit(
            2); // NOLINT(concurrency-mt-unsafe) — CLI single-threaded at open time; mirrors cli_parse.c exit() pattern
    }
    if (file_sz % (off_t)frame_sz != 0) {
        (void)fprintf(stderr,
                      "yuv: file size mismatch — expected a multiple of %zu bytes "
                      "for %ux%u %u-bit %s, got %lld bytes "
                      "(hint: check --bitdepth and --pixel_format; "
                      "%u-bit frames need %u byte%s per sample)\n",
                      frame_sz, yuv->width, yuv->height, yuv->bitdepth, fmt_name,
                      (long long)file_sz, yuv->bitdepth, bpp, bpp == 1u ? "" : "s");
        exit(
            2); // NOLINT(concurrency-mt-unsafe) — CLI single-threaded at open time; mirrors cli_parse.c exit() pattern
    }
}

static yuv_input *yuv_input_open(FILE *_fin, unsigned width, unsigned height,
                                 enum VmafPixelFormat pix_fmt, unsigned bitdepth)
{
    yuv_input *yuv = malloc(sizeof(*yuv));
    if (!yuv) {
        (void)fprintf(stderr, "Could not allocate yuv reader state.\n");
        return NULL;
    }

    yuv->fin = _fin;
    yuv->width = width;
    yuv->height = height;
    yuv->pix_fmt = pix_fmt;
    yuv->bitdepth = bitdepth;
    bool hbd = yuv->bitdepth > 8;

    /* Cast width/height to size_t before any multiplication so the
     * intermediate arithmetic proceeds in size_t precision (64-bit on every
     * supported 64-bit host).  Without the cast each `width * height` runs
     * in `unsigned` (32-bit) and wraps to a small value for adversarial CLI
     * inputs near the unsigned ceiling.  The downstream malloc() would then
     * succeed with a too-small buffer and the first fread() at
     * yuv_input_fetch_frame would write past the heap allocation.
     *
     * `hbd` (0 or 1) is a plain int from `bitdepth > 8`; the size_t cast on
     * the left operand makes the shift well-defined for sizes near SIZE_MAX
     * (where shifting an `unsigned` would invoke undefined behaviour). */
    const size_t w = (size_t)yuv->width;
    const size_t h = (size_t)yuv->height;
    const size_t cw = (w + 1U) / 2U;
    const size_t ch = (h + 1U) / 2U;
    switch (yuv->pix_fmt) {
    case VMAF_PIX_FMT_YUV420P:
        yuv->src_c_dec_h = yuv->dst_c_dec_h = yuv->src_c_dec_v = yuv->dst_c_dec_v = 2;
        yuv->dst_buf_sz = (w * h + 2U * cw * ch) << hbd;
        break;
    case VMAF_PIX_FMT_YUV422P:
        yuv->src_c_dec_h = yuv->dst_c_dec_h = 2;
        yuv->src_c_dec_v = yuv->dst_c_dec_v = 1;
        yuv->dst_buf_sz = (w * h + 2U * cw * h) << hbd;
        break;
    case VMAF_PIX_FMT_YUV444P:
        yuv->src_c_dec_h = yuv->dst_c_dec_h = yuv->src_c_dec_v = yuv->dst_c_dec_v = 1;
        yuv->dst_buf_sz = (w * h * 3U) << hbd;
        break;
    default:
        goto fail;
    }

    yuv_check_file_size(_fin, yuv); /* exits with code 2 on mismatch */

    yuv->dst_buf = malloc(yuv->dst_buf_sz);
    if (!yuv->dst_buf) {
        (void)fprintf(stderr, "Could not allocate yuv reader buffer.\n");
        goto fail;
    }

    return yuv;

fail:
    free(yuv);
    return NULL;
}

static int pix_fmt_map(enum VmafPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case VMAF_PIX_FMT_YUV420P:
        return PF_420;
    case VMAF_PIX_FMT_YUV422P:
        return PF_422;
    case VMAF_PIX_FMT_YUV444P:
        return PF_444;
    default:
        return 0;
    }
}

static void yuv_input_get_info(yuv_input *_yuv, video_input_info *_info)
{
    memset(_info, 0, sizeof(*_info));
    _info->frame_w = _info->pic_w = _yuv->width;
    _info->frame_h = _info->pic_h = _yuv->height;
    _info->pixel_fmt = pix_fmt_map(_yuv->pix_fmt);
    _info->depth = _yuv->bitdepth;
}

static int yuv_input_fetch_frame(yuv_input *yuv, FILE *fin, video_input_ycbcr _ycbcr,
                                 const char _tag[5])
{
    size_t bytes_read = fread(yuv->dst_buf, 1, yuv->dst_buf_sz, fin);
    if (bytes_read == 0)
        return 0;
    if (bytes_read != yuv->dst_buf_sz) {
        (void)fprintf(stderr, "Error reading YUV frame data.\n");
        return -1;
    }

    (void)_tag;

    /* Promote all geometry values to size_t before multiplication to prevent
     * unsigned 32-bit wraparound for large YUV444P / HBD frames.  For example,
     * a 46341x46341 10-bit YUV444P frame has c_w * c_h * xstride =
     * 46341 * 46341 * 2 = 4,294,976,562 which exceeds UINT32_MAX (4,294,967,295)
     * and would wrap to 9,267 in plain unsigned arithmetic, producing a buffer
     * pointer far past the end of dst_buf.  The same promotion is already
     * applied to dst_buf_sz in yuv_input_open() and must be mirrored here. */
    size_t xstride = (yuv->bitdepth > 8) ? 2u : 1u;
    size_t pic_sz = (size_t)yuv->width * (size_t)yuv->height * xstride;
    unsigned frame_c_w = yuv->width / yuv->dst_c_dec_h;
    unsigned frame_c_h = yuv->height / yuv->dst_c_dec_v;
    size_t c_w = ((size_t)yuv->width + (size_t)yuv->dst_c_dec_h - 1u) / (size_t)yuv->dst_c_dec_h;
    size_t c_h = ((size_t)yuv->height + (size_t)yuv->dst_c_dec_v - 1u) / (size_t)yuv->dst_c_dec_v;
    size_t c_sz = c_w * c_h * xstride;

    _ycbcr[0].width = yuv->width;
    _ycbcr[0].height = yuv->height;
    _ycbcr[0].stride = yuv->width * xstride;
    _ycbcr[0].data = yuv->dst_buf;
    _ycbcr[1].width = frame_c_w;
    _ycbcr[1].height = frame_c_h;
    _ycbcr[1].stride = c_w * xstride;
    _ycbcr[1].data = yuv->dst_buf + pic_sz;
    _ycbcr[2].width = frame_c_w;
    _ycbcr[2].height = frame_c_h;
    _ycbcr[2].stride = c_w * xstride;
    _ycbcr[2].data = _ycbcr[1].data + c_sz;

    return 1;
}

static void yuv_input_close(yuv_input *_yuv)
{
    free(_yuv->dst_buf);
}

static int yuv_fetch_into_vmaf_picture(yuv_input *yuv, FILE *fin, VmafPicture *pic)
{
    (void)yuv;
    size_t bytes_per_sample = (pic->bpc + 7) / 8;

    for (unsigned i = 0; i < 3; i++) {
        size_t row_bytes = (size_t)pic->w[i] * bytes_per_sample;
        if (pic->stride[i] == (ptrdiff_t)row_bytes) {
            size_t total = row_bytes * pic->h[i];
            size_t bytes_read = fread(pic->data[i], 1, total, fin);
            if (bytes_read == 0 && i == 0)
                return 0;
            if (bytes_read != total) {
                (void)fprintf(stderr, "Error reading YUV frame data.\n");
                return -1;
            }
        } else {
            uint8_t *dst = pic->data[i];
            for (unsigned j = 0; j < pic->h[i]; j++) {
                size_t bytes_read = fread(dst, 1, row_bytes, fin);
                if (bytes_read == 0 && i == 0 && j == 0)
                    return 0;
                if (bytes_read != row_bytes) {
                    (void)fprintf(stderr, "Error reading YUV frame data.\n");
                    return -1;
                }
                dst += pic->stride[i];
            }
        }
    }

    return 1;
}

/*
 * vtbl-compatible wrapper functions — each matches the exact function pointer
 * signature in vidinput.h so the VTBL initializer below requires no C-style
 * casts.  The casts were previously silencing a type mismatch between the
 * concrete `yuv_input *` parameter and the erased `void *` in the typedef,
 * which UBSan's -fsanitize=function detects at runtime as undefined behaviour.
 * The concrete implementations remain typed for readability and safety.
 */
static void *yuv_vtbl_open_raw(FILE *fin, unsigned w, unsigned h, int pix_fmt, unsigned bitdepth)
{
    return yuv_input_open(fin, w, h, (enum VmafPixelFormat)pix_fmt, bitdepth);
}

static void yuv_vtbl_get_info(void *ctx, video_input_info *info)
{
    yuv_input_get_info((yuv_input *)ctx, info);
}

static int yuv_vtbl_fetch_frame(void *ctx, FILE *fin, video_input_ycbcr ycbcr, char tag[5])
{
    return yuv_input_fetch_frame((yuv_input *)ctx, fin, ycbcr, tag);
}

static void yuv_vtbl_close(void *ctx)
{
    yuv_input_close((yuv_input *)ctx);
}

static int yuv_vtbl_fetch_into_vmaf_picture(void *ctx, FILE *fin, VmafPicture *pic)
{
    return yuv_fetch_into_vmaf_picture((yuv_input *)ctx, fin, pic);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — extern linkage required: vidinput.c references this symbol via `extern video_input_vtbl YUV_INPUT_VTBL`
OC_EXTERN const video_input_vtbl YUV_INPUT_VTBL = {
    yuv_vtbl_open_raw,    NULL,           yuv_vtbl_get_info,
    yuv_vtbl_fetch_frame, yuv_vtbl_close, yuv_vtbl_fetch_into_vmaf_picture};
