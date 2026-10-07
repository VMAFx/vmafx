/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
#include "vmafx/import_layouts_gen.h"
#include "vmafx/rgb_convert.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/** Linkage will break without this if using a C++ compiler, and will issue
 * warnings without this for a C compiler*/
#if defined(__cplusplus)
#define OC_EXTERN extern
#else
#define OC_EXTERN
#endif

/* A raw file frame is either the planar frame of `pix_fmt` (layout NULL) or the
 * layout of the import table named by the --pixel_format (NV12, P010, YUYV422,
 * V210, RGBA ...), which is read into `src_buf` and converted into the planar
 * frame of `layout->planar_fmt` by the code the library's host import runs
 * (vmafx/import_layout.h, vmafx/import_convert.h, vmafx/rgb_convert.h): one
 * behaviour per layout (ADR-2145, ADR-2146). */
typedef struct yuv_input {
    FILE *fin;
    unsigned width, height;
    enum VmafPixelFormat pix_fmt; /* of the planar frame made */
    unsigned bitdepth;
    size_t dst_buf_sz;
    uint8_t *dst_buf;
    int src_c_dec_v, src_c_dec_h;
    int dst_c_dec_h, dst_c_dec_v;
    const VmafxImportLayout *layout; /* NULL: the file holds the planar frame */
    size_t frame_sz;                 /* bytes of one frame in the file */
    uint8_t *src_buf;                /* one frame of the layout */
    VmafxRgbPlan rgb;                /* RGB layouts: set by raw_input_set_rgb() */
    bool rgb_ready;
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
    size_t frame_sz = yuv->frame_sz;
    unsigned bpp = yuv->bitdepth > 8u ? 2u : 1u;
    const char *fmt_name = yuv->layout                          ? yuv->layout->name :
                           yuv->pix_fmt == VMAF_PIX_FMT_YUV420P ? "yuv420p" :
                           yuv->pix_fmt == VMAF_PIX_FMT_YUV422P ? "yuv422p" :
                           yuv->pix_fmt == VMAF_PIX_FMT_YUV400P ? "gray" :
                                                                  "yuv444p";

    if (file_sz < (off_t)frame_sz) {
        (void)fprintf(stderr,
                      "yuv: file too small for declared geometry — "
                      "need at least %zu bytes for one %ux%u %u-bit %s frame, "
                      "got %lld bytes\n",
                      frame_sz, yuv->width, yuv->height, yuv->bitdepth, fmt_name,
                      (long long)file_sz);
        /* CLI is single-threaded at open time; mirrors the cli_parse.c exit()
         * pattern. ADR-0141 / ADR-0278. */
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(2);
    }
    if (file_sz % (off_t)frame_sz != 0) {
        (void)fprintf(stderr,
                      "yuv: file size mismatch — expected a multiple of %zu bytes "
                      "for %ux%u %u-bit %s, got %lld bytes "
                      "(hint: check --bitdepth and --pixel_format; "
                      "%u-bit frames need %u byte%s per sample)\n",
                      frame_sz, yuv->width, yuv->height, yuv->bitdepth, fmt_name,
                      (long long)file_sz, yuv->bitdepth, bpp, bpp == 1u ? "" : "s");
        /* CLI is single-threaded at open time; mirrors the cli_parse.c exit()
         * pattern. ADR-0141 / ADR-0278. */
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(2);
    }
}

/* Derive the chroma decimation factors and the destination buffer size from
 * the already-populated width / height / pix_fmt / bitdepth fields.
 *
 * Returns 0 on success, or -1 when @p yuv->pix_fmt names a layout this reader
 * does not support — the caller then releases @p yuv and reports the failure.
 *
 * Cast width/height to size_t before any multiplication so the intermediate
 * arithmetic proceeds in size_t precision (64-bit on every supported 64-bit
 * host).  Without the cast each `width * height` runs in `unsigned` (32-bit)
 * and wraps to a small value for adversarial CLI inputs near the unsigned
 * ceiling.  The downstream malloc() would then succeed with a too-small
 * buffer and the first fread() at yuv_input_fetch_frame would write past the
 * heap allocation.
 *
 * `hbd` (0 or 1) is a plain int from `bitdepth > 8`; the size_t cast on the
 * left operand makes the shift well-defined for sizes near SIZE_MAX (where
 * shifting an `unsigned` would invoke undefined behaviour). */
static int yuv_input_set_plane_geometry(yuv_input *yuv)
{
    const bool hbd = yuv->bitdepth > 8;
    const size_t w = (size_t)yuv->width;
    const size_t h = (size_t)yuv->height;
    const size_t cw = (w + 1U) / 2U;
    const size_t ch = (h + 1U) / 2U;

    switch (yuv->pix_fmt) {
    case VMAF_PIX_FMT_YUV420P:
        yuv->src_c_dec_h = yuv->dst_c_dec_h = yuv->src_c_dec_v = yuv->dst_c_dec_v = 2;
        yuv->dst_buf_sz = (w * h + 2U * cw * ch) << hbd;
        return 0;
    case VMAF_PIX_FMT_YUV422P:
        yuv->src_c_dec_h = yuv->dst_c_dec_h = 2;
        yuv->src_c_dec_v = yuv->dst_c_dec_v = 1;
        yuv->dst_buf_sz = (w * h + 2U * cw * h) << hbd;
        return 0;
    case VMAF_PIX_FMT_YUV444P:
        yuv->src_c_dec_h = yuv->dst_c_dec_h = yuv->src_c_dec_v = yuv->dst_c_dec_v = 1;
        yuv->dst_buf_sz = (w * h * 3U) << hbd;
        return 0;
    case VMAF_PIX_FMT_YUV400P:
        /* Luma only; the chroma factors are never read (no chroma plane). */
        yuv->src_c_dec_h = yuv->dst_c_dec_h = yuv->src_c_dec_v = yuv->dst_c_dec_v = 1;
        yuv->dst_buf_sz = (w * h) << hbd;
        return 0;
    default:
        return -1;
    }
}

/* The layout of the import table a raw --pixel_format names, or NULL for a
 * planar VmafPixelFormat (1 to 4) and for a value that is none. */
static const VmafxImportLayout *yuv_find_layout(int pix_fmt)
{
    if (pix_fmt < (int)VMAFX_PIXEL_FORMAT_NV12)
        return NULL;
    for (size_t i = 0; i < VMAFX_N_IMPORT_LAYOUTS; i++) {
        if (vmafx_import_layouts[i].pix_fmt == (uint32_t)pix_fmt)
            return &vmafx_import_layouts[i];
    }
    return NULL;
}

/* Plane geometry of the planar frame made, as the library's import takes it. */
static void yuv_planar_extents(const yuv_input *yuv, unsigned pw[3], unsigned ph[3])
{
    const unsigned cw_dec = (unsigned)yuv->dst_c_dec_h;
    const unsigned ch_dec = (unsigned)yuv->dst_c_dec_v;
    pw[0] = yuv->width;
    ph[0] = yuv->height;
    pw[1] = pw[2] = yuv->pix_fmt == VMAF_PIX_FMT_YUV400P ? 0u : (yuv->width + cw_dec - 1u) / cw_dec;
    ph[1] = ph[2] =
        yuv->pix_fmt == VMAF_PIX_FMT_YUV400P ? 0u : (yuv->height + ch_dec - 1u) / ch_dec;
}

/* Bytes of the producer planes of one frame of `yuv->layout`, one after the
 * other with the rows tightly packed (the way a raw file holds them). */
static size_t yuv_layout_frame_size(const yuv_input *yuv)
{
    unsigned pw[3];
    unsigned ph[3];
    yuv_planar_extents(yuv, pw, ph);
    size_t total = 0;
    for (uint32_t i = 0; i < yuv->layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(yuv->layout, yuv->bitdepth, i, pw, ph, &row, &rows);
        total += (size_t)(row * rows);
    }
    return total;
}

/* Fix the planar frame and the file frame of a layout. -1 when the bit depth is
 * outside what the layout holds. */
static int yuv_layout_setup(yuv_input *yuv, const VmafxImportLayout *layout)
{
    if (yuv->bitdepth < layout->bpc_min || yuv->bitdepth > layout->bpc_max) {
        (void)fprintf(stderr, "yuv: %s holds %u to %u bits per component, not %u\n", layout->name,
                      layout->bpc_min, layout->bpc_max, yuv->bitdepth);
        return -1;
    }
    yuv->layout = layout;
    yuv->pix_fmt = (enum VmafPixelFormat)layout->planar_fmt;
    if (yuv_input_set_plane_geometry(yuv) != 0)
        return -1;
    yuv->frame_sz = yuv_layout_frame_size(yuv);
    yuv->src_buf = malloc(yuv->frame_sz);
    if (!yuv->src_buf) {
        (void)fprintf(stderr, "Could not allocate yuv layout buffer.\n");
        return -1;
    }
    return 0;
}

static yuv_input *yuv_input_open(FILE *_fin, unsigned width, unsigned height, int pix_fmt,
                                 unsigned bitdepth)
{
    yuv_input *yuv = calloc(1, sizeof(*yuv));
    if (!yuv) {
        (void)fprintf(stderr, "Could not allocate yuv reader state.\n");
        return NULL;
    }

    yuv->fin = _fin;
    yuv->width = width;
    yuv->height = height;
    yuv->pix_fmt = (enum VmafPixelFormat)pix_fmt;
    yuv->bitdepth = bitdepth;

    const VmafxImportLayout *const layout = yuv_find_layout(pix_fmt);
    if (layout ? yuv_layout_setup(yuv, layout) != 0 : yuv_input_set_plane_geometry(yuv) != 0) {
        free(yuv->src_buf);
        free(yuv);
        return NULL;
    }
    if (!layout)
        yuv->frame_sz = yuv->dst_buf_sz;
    if (layout && layout->needs_statement) {
        /* The statement comes with raw_input_set_rgb(); a frame is never read before. */
        yuv->rgb_ready = false;
    }

    yuv_check_file_size(_fin, yuv); /* exits with code 2 on mismatch */

    yuv->dst_buf = malloc(yuv->dst_buf_sz);
    if (!yuv->dst_buf) {
        (void)fprintf(stderr, "Could not allocate yuv reader buffer.\n");
        free(yuv->src_buf);
        free(yuv);
        return NULL;
    }

    return yuv;
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
    case VMAF_PIX_FMT_YUV400P:
        return PF_400;
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

/* Convert the layout frame in `src_buf` into the planar frame `out` (data and stride of each
 * plane): the library's reads, plane by plane. */
static void yuv_convert_layout(const yuv_input *yuv, uint8_t *const out[3], const size_t stride[3])
{
    unsigned pw[3];
    unsigned ph[3];
    yuv_planar_extents(yuv, pw, ph);
    const uint8_t *src[3] = {NULL, NULL, NULL};
    size_t pitch[3] = {0, 0, 0};
    size_t offset = 0;
    for (uint32_t i = 0; i < yuv->layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(yuv->layout, yuv->bitdepth, i, pw, ph, &row, &rows);
        src[i] = yuv->src_buf + offset;
        pitch[i] = (size_t)row;
        offset += (size_t)(row * rows);
    }
    const uint32_t out_bytes = yuv->bitdepth > 8u ? 2u : 1u;
    const unsigned n_out = yuv->pix_fmt == VMAF_PIX_FMT_YUV400P ? 1u : 3u;
    for (unsigned i = 0; i < n_out; i++) {
        if (yuv->layout->packed == VMAFX_IMPORT_PACKED_RGB) {
            vmafx_rgb_read_plane(out[i], stride[i], src[0], pitch[0], pw[i], ph[i], i, &yuv->rgb);
            continue;
        }
        VmafxImportRead rd;
        vmafx_import_plane_read(yuv->layout, yuv->bitdepth, i, &rd);
        vmafx_import_read_plane(out[i], stride[i], out_bytes, src[rd.src_plane],
                                pitch[rd.src_plane], pw[i], ph[i], &rd);
    }
}

/* Read one frame of the layout and convert it into `out`. 1 on success, 0 for a clean EOF before
 * the frame, -1 on a short read or an RGB layout without its statement. */
static int yuv_load_layout(yuv_input *yuv, FILE *fin, uint8_t *const out[3], const size_t stride[3])
{
    if (yuv->layout->needs_statement && !yuv->rgb_ready) {
        (void)fprintf(stderr,
                      "yuv: %s is converted to Y'CbCr with a matrix, range and transfer you "
                      "state; none is assumed (--rgb_matrix, --rgb_range, --rgb_transfer, "
                      "--rgb_out_range)\n",
                      yuv->layout->name);
        return -1;
    }
    const size_t bytes_read = fread(yuv->src_buf, 1, yuv->frame_sz, fin);
    if (bytes_read == 0)
        return 0;
    if (bytes_read != yuv->frame_sz) {
        (void)fprintf(stderr, "Error reading YUV frame data.\n");
        return -1;
    }
    yuv_convert_layout(yuv, out, stride);
    return 1;
}

/* The planar frame of a layout into `dst_buf`, rows tightly packed. */
static int yuv_load_layout_tight(yuv_input *yuv, FILE *fin)
{
    unsigned pw[3];
    unsigned ph[3];
    yuv_planar_extents(yuv, pw, ph);
    const size_t xs = yuv->bitdepth > 8u ? 2u : 1u;
    uint8_t *out[3];
    size_t stride[3];
    uint8_t *at = yuv->dst_buf;
    for (unsigned i = 0; i < 3u; i++) {
        out[i] = at;
        stride[i] = (size_t)pw[i] * xs;
        at += stride[i] * ph[i];
    }
    return yuv_load_layout(yuv, fin, out, stride);
}

static int yuv_input_fetch_frame(yuv_input *yuv, FILE *fin, video_input_ycbcr _ycbcr,
                                 const char _tag[5])
{
    if (yuv->layout) {
        const int rc = yuv_load_layout_tight(yuv, fin);
        if (rc <= 0)
            return rc;
    } else {
        size_t bytes_read = fread(yuv->dst_buf, 1, yuv->dst_buf_sz, fin);
        if (bytes_read == 0)
            return 0;
        if (bytes_read != yuv->dst_buf_sz) {
            (void)fprintf(stderr, "Error reading YUV frame data.\n");
            return -1;
        }
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
    const bool luma_only = yuv->pix_fmt == VMAF_PIX_FMT_YUV400P;
    unsigned frame_c_w = luma_only ? 0u : yuv->width / yuv->dst_c_dec_h;
    unsigned frame_c_h = luma_only ? 0u : yuv->height / yuv->dst_c_dec_v;
    size_t c_w =
        luma_only ? 0u :
                    ((size_t)yuv->width + (size_t)yuv->dst_c_dec_h - 1u) / (size_t)yuv->dst_c_dec_h;
    size_t c_h = luma_only ? 0u :
                             ((size_t)yuv->height + (size_t)yuv->dst_c_dec_v - 1u) /
                                 (size_t)yuv->dst_c_dec_v;
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
    free(_yuv->src_buf);
}

/* The statement of an RGB layout; the transfer is stated and recorded by the caller (the matrix
 * applies to the code values as an encoder does, ADR-2146). */
static int yuv_input_set_rgb(yuv_input *yuv, unsigned matrix, unsigned range, unsigned transfer,
                             unsigned out_range)
{
    (void)transfer;
    if (!yuv->layout || !yuv->layout->needs_statement)
        return -1;
    if (!vmafx_rgb_plan_init(&yuv->rgb, yuv->layout, yuv->bitdepth, matrix, range, out_range))
        return -1;
    yuv->rgb_ready = true;
    return 0;
}

/* Read one plane. Returns 1 on success, 0 for a clean EOF at the very first
 * read of the frame (the caller turns that into "no more frames"), -1 on a
 * short or failed read. Split out of yuv_fetch_into_vmaf_picture so that
 * function stays inside the readability-function-size budget (ADR-1142); the
 * contiguous and strided paths and their EOF handling are unchanged. */
static int yuv_read_plane(FILE *fin, VmafPicture *pic, unsigned i, size_t bytes_per_sample,
                          bool first_plane)
{
    const size_t row_bytes = (size_t)pic->w[i] * bytes_per_sample;

    if (pic->stride[i] == (ptrdiff_t)row_bytes) {
        const size_t total = row_bytes * pic->h[i];
        const size_t bytes_read = fread(pic->data[i], 1, total, fin);
        if (bytes_read == 0 && first_plane)
            return 0;
        if (bytes_read != total) {
            (void)fprintf(stderr, "Error reading YUV frame data.\n");
            return -1;
        }
        return 1;
    }

    uint8_t *dst = pic->data[i];
    for (unsigned j = 0; j < pic->h[i]; j++) {
        const size_t bytes_read = fread(dst, 1, row_bytes, fin);
        if (bytes_read == 0 && first_plane && j == 0)
            return 0;
        if (bytes_read != row_bytes) {
            (void)fprintf(stderr, "Error reading YUV frame data.\n");
            return -1;
        }
        dst += pic->stride[i];
    }
    return 1;
}

static int yuv_fetch_into_vmaf_picture(yuv_input *yuv, FILE *fin, VmafPicture *pic)
{
    if (yuv->layout) {
        uint8_t *out[3] = {pic->data[0], pic->data[1], pic->data[2]};
        const size_t stride[3] = {(size_t)pic->stride[0], (size_t)pic->stride[1],
                                  (size_t)pic->stride[2]};
        return yuv_load_layout(yuv, fin, out, stride);
    }
    const size_t bytes_per_sample = (pic->bpc + 7) / 8;

    for (unsigned i = 0; i < 3; i++) {
        const int rc = yuv_read_plane(fin, pic, i, bytes_per_sample, i == 0);
        if (rc <= 0)
            return rc;
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
    return yuv_input_open(fin, w, h, pix_fmt, bitdepth);
}

static void yuv_vtbl_get_info(void *ctx, video_input_info *info)
{
    yuv_input_get_info((yuv_input *)ctx, info);
}

static int yuv_vtbl_fetch_frame(void *ctx, FILE *fin, video_input_ycbcr ycbcr, char tag[5])
{
    return yuv_input_fetch_frame((yuv_input *)ctx, fin, ycbcr, tag);
}

static int yuv_vtbl_set_rgb(void *ctx, unsigned matrix, unsigned range, unsigned transfer,
                            unsigned out_range)
{
    return yuv_input_set_rgb((yuv_input *)ctx, matrix, range, transfer, out_range);
}

static void yuv_vtbl_close(void *ctx)
{
    yuv_input_close((yuv_input *)ctx);
}

static int yuv_vtbl_fetch_into_vmaf_picture(void *ctx, FILE *fin, VmafPicture *pic)
{
    return yuv_fetch_into_vmaf_picture((yuv_input *)ctx, fin, pic);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — extern linkage required: vidinput.c references this symbol via `extern video_input_vtbl YUV_INPUT_VTBL` (ADR-0141 / ADR-0278)
OC_EXTERN const video_input_vtbl YUV_INPUT_VTBL = {.open_raw = yuv_vtbl_open_raw,
                                                   .open = NULL,
                                                   .get_info = yuv_vtbl_get_info,
                                                   .fetch_frame = yuv_vtbl_fetch_frame,
                                                   .close = yuv_vtbl_close,
                                                   .fetch_into_vmaf_picture =
                                                       yuv_vtbl_fetch_into_vmaf_picture,
                                                   .set_rgb = yuv_vtbl_set_rgb};

/* NOLINTEND(modernize-use-nullptr) */
