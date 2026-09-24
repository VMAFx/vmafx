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

/* ADR-0809 — C++23 Wave 8: vmaf.c → vmaf.cpp.
 * Conservative idioms: nullptr, static_cast and [[nodiscard]]. CliRunState
 * owns every resource acquired after option parsing; CliRunGuard performs the
 * single ordered teardown on all returns. Spinner header uses inline to
 * suppress ODR warnings. */

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#ifdef _WIN32
/* MSVC/UCRT provides isatty / fileno via <io.h> under the MSVC-prefixed
 * names _isatty / _fileno; the POSIX-style aliases stay available for
 * source portability. MinGW ships <unistd.h>, so this split is strictly
 * MSVC / clang-cl. */
#include <io.h>
#include <windows.h> /* QueryPerformanceCounter/Frequency for wall_time_s() */
#define isatty _isatty
#define fileno _fileno
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "cli_parse.h"
#include "compat/path_utf8.h"
#include "spinner.h"
#include "vidinput.h"

#include "libvmaf/picture.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/dnn.h"
#ifdef HAVE_CUDA
#include "libvmaf/libvmaf_cuda.h"
#endif
#ifdef HAVE_SYCL
#include "libvmaf/libvmaf_sycl.h"
#endif
#ifdef HAVE_HIP
#include "libvmaf/libvmaf_hip.h"
#endif
#ifdef HAVE_METAL
#include "libvmaf/libvmaf_metal.h"
#endif

#include "feature/feature_dimensions.h"

/* ADR-0543 (extends ADR-0498): dedicated exit code for an explicit-
 * backend init failure. Distinguishes a "you asked for SYCL but it
 * couldn't initialise" failure from generic encode / score errors
 * (which keep using non-zero-but-unspecified) so CI gates and the
 * vmaf-tune bisect predicate can tell the two apart without parsing
 * stderr. Mirrors the EX_* convention from <sysexits.h>; we don't pull
 * sysexits.h in to stay portable to Windows/MSVC. */
#define VMAF_EXIT_BACKEND_INIT_FAILED 100

/* Dedicated exit code for "no frames decoded": run_frame_loop returned 0
 * because neither input yielded a single frame (empty file, zero-byte pipe,
 * frame_skip past EOF, or a fully unreadable stream). Without this guard the
 * pooling path computes `picture_index - 1`, which underflows the unsigned
 * counter to UINT_MAX and feeds a bogus index range into vmaf_score_pooled.
 * Distinct from VMAF_EXIT_BACKEND_INIT_FAILED so CI gates can tell an
 * empty/short input apart from a backend failure. */
#define VMAF_EXIT_NO_FRAMES_DECODED 101

/* ADR-1262: dedicated exit code for "an input stream failed to read".
 * Distinct from VMAF_EXIT_NO_FRAMES_DECODED, which means the inputs were
 * merely empty or too short: 102 means bytes were expected and the read
 * failed, so whatever frames were consumed are a truncated prefix and any
 * pooled score over them is computed on less data than the caller asked for.
 * Before ADR-1262 every read failure left the exit status at 0, so a corrupt
 * pair was indistinguishable from a clean short one to any caller that tests
 * `$?` rather than scraping stderr. A stream that simply ends earlier than
 * its partner is NOT this: that stays a warning and exit 0, because scoring
 * the common prefix of a legitimately shorter file is a supported use. */
#define VMAF_EXIT_INPUT_READ_ERROR 102

/* ADR-0543: sentinel returned by init_gpu_backends when the user passed
 * `--backend NAME` (non-auto/cpu) and that backend failed to initialise.
 * The caller in main() distinguishes this from the generic `-1` path so
 * (a) the binary exits with VMAF_EXIT_BACKEND_INIT_FAILED rather than the
 * default 255 (= int -1 → uint8), and (b) the JSON output path, if
 * provided, is overwritten with an `{"error": ..., "backend_requested": ...}`
 * descriptor so downstream consumers see a structured failure instead of an
 * empty file. The value is deliberately distinct from any errno-style
 * negative returned by the underlying vmaf_*_state_init helpers (which
 * are -EINVAL / -ENODEV / -ENOMEM range, well above -100 in magnitude
 * for typical errno but never exactly -100 in practice). */
#define VMAF_INIT_GPU_EXPLICIT_FAIL (-100)

namespace
{

enum VmafPixelFormat pix_fmt_map(int pf)
{
    switch (pf) {
    case PF_420:
        return VMAF_PIX_FMT_YUV420P;
    case PF_422:
        return VMAF_PIX_FMT_YUV422P;
    case PF_444:
        return VMAF_PIX_FMT_YUV444P;
    default:
        return VMAF_PIX_FMT_UNKNOWN;
    }
}

} // namespace

/* ADR-0543 (extends ADR-0498): when the explicit-backend gate fires we
 * overwrite the requested ``--output`` file (if any) with a minimal JSON
 * descriptor so downstream consumers (CI gates, vmaf-tune compare,
 * MCP probes) get a structured signal instead of an empty file. The
 * file is overwritten unconditionally — empty / partial / pre-existing
 * content is replaced. No-op when output_path is NULL or the requested
 * format isn't JSON; XML / CSV / SUB consumers don't read the error
 * field, but the non-zero exit code still surfaces the failure.
 *
 * Schema (RFC 8259 strict):
 *   {
 *     "error": "<human-readable reason>",
 *     "backend_requested": "<sycl|cuda|hip|metal>",
 *     "errno": <int>,
 *     "adr": "ADR-0498",
 *     "exit_code": 100
 *   }
 *
 * `err_no` is the underlying ``vmaf_*_state_init`` return (negative
 * errno-style) or 0 when the failure is structural (e.g. backend not
 * compiled in).
 */
namespace
{

void write_backend_error_json(const char *output_path, enum VmafOutputFormat fmt,
                              const char *backend_requested, const char *reason, int err_no)
{
    if (!output_path || !backend_requested || !reason)
        return;
    if (fmt != VMAF_OUTPUT_FORMAT_JSON)
        return;

    /* Use open()+fdopen() with explicit 0644 mode so the created file is never
     * world-writable regardless of the caller's umask (CodeQL cpp/world-writable-file-creation). */
#ifdef _WIN32
    FILE *fp = vmaf_fopen_utf8(output_path, "wb");
#else
    const int raw_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    FILE *fp = (raw_fd >= 0) ? fdopen(raw_fd, "wb") : nullptr;
    if (!fp && raw_fd >= 0) {
        /* POSIX leaves the descriptor open when fdopen() fails, so closing it here is
         * required.  cppcheck's posix.cfg lists fdopen as a deallocator of the fd
         * unconditionally, so 2.13 — the version CI installs from apt — reads this as a
         * second free.  2.21 no longer does. */
        /* cppcheck-suppress doubleFree ; see the note above */
        (void)close(raw_fd);
    }
#endif
    if (!fp)
        return;
    /* Keep the JSON compact + single line — every consumer in the tree
     * parses it with a permissive reader and the file is short. */
    (void)fprintf(fp,
                  "{\"error\": \"%s\", \"backend_requested\": \"%s\", "
                  "\"errno\": %d, \"adr\": \"ADR-0498\", "
                  "\"exit_code\": %d}\n",
                  reason, backend_requested, err_no, VMAF_EXIT_BACKEND_INIT_FAILED);
    (void)fclose(fp);
}

} // namespace

/* Validate per-video constraints that do not require comparing the two streams:
 * supported bitdepth range and positive (non-zero) frame dimensions. */
namespace
{

[[nodiscard]] int validate_video_info(const video_input_info *info)
{
    int err_cnt = 0;

    if (info->depth < 8 || info->depth > 16) {
        (void)fprintf(stderr, "unsupported bitdepth: %d\n", info->depth);
        err_cnt++;
    }

    /* A zero-width or zero-height frame will produce a divide-by-zero or
     * zero-stride allocation in downstream code. */
    if (info->frame_w <= 0 || info->frame_h <= 0) {
        (void)fprintf(stderr, "non-positive dimensions: %dx%d\n", info->frame_w, info->frame_h);
        err_cnt++;
    }

    return err_cnt;
}

/* Chroma-subsampled formats require even dimensions on the subsampled axes so
 * that the chroma planes contain whole pixels.  PF_420 subsamples both X and
 * Y; PF_422 subsamples X only. */
[[nodiscard]] int validate_chroma_alignment(const video_input_info *info)
{
    int err_cnt = 0;

    if (info->pixel_fmt == PF_420 || info->pixel_fmt == PF_422) {
        if (info->frame_w % 2 != 0) {
            (void)fprintf(stderr, "odd width %d not allowed for chroma-subsampled format\n",
                          info->frame_w);
            err_cnt++;
        }
    }
    if (info->pixel_fmt == PF_420) {
        if (info->frame_h % 2 != 0) {
            (void)fprintf(stderr, "odd height %d not allowed for 4:2:0 format\n", info->frame_h);
            err_cnt++;
        }
    }

    return err_cnt;
}

} // namespace

namespace
{

[[nodiscard]] int validate_videos(video_input *vid1, video_input *vid2, bool common_bitdepth)
{
    int err_cnt = 0;

    video_input_info info1;
    video_input_info info2;
    video_input_get_info(vid1, &info1);
    video_input_get_info(vid2, &info2);

    if ((info1.frame_w != info2.frame_w) || (info1.frame_h != info2.frame_h)) {
        (void)fprintf(stderr, "dimensions do not match: %dx%d, %dx%d\n", info1.frame_w,
                      info1.frame_h, info2.frame_w, info2.frame_h);
        err_cnt++;
    }

    if (info1.pixel_fmt != info2.pixel_fmt) {
        (void)fprintf(stderr, "pixel formats do not match: %d, %d\n", info1.pixel_fmt,
                      info2.pixel_fmt);
        err_cnt++;
    }

    if (!pix_fmt_map(info1.pixel_fmt) || !pix_fmt_map(info2.pixel_fmt)) {
        (void)fprintf(stderr, "unsupported pixel format: %d\n", info1.pixel_fmt);
        err_cnt++;
    }

    if (!common_bitdepth && info1.depth != info2.depth) {
        (void)fprintf(stderr, "bitdepths do not match: %d, %d\n", info1.depth, info2.depth);
        err_cnt++;
    }

    err_cnt += validate_video_info(&info1);
    err_cnt += validate_video_info(&info2);
    err_cnt += validate_chroma_alignment(&info1);

    return err_cnt;
}

} // namespace

namespace
{

void copy_equal_depth_8(VmafPicture *pic, video_input_ycbcr ycbcr, const video_input_info *info)
{
    for (unsigned i = 0; i < 3; i++) {
        const int xdec = i && !(info->pixel_fmt & 1);
        const int ydec = i && !(info->pixel_fmt & 2);
        const uint8_t *ycbcr_data = ycbcr[i].data +
                                    static_cast<size_t>(info->pic_y >> ydec) * ycbcr[i].stride +
                                    (info->pic_x >> xdec);
        uint8_t *pic_data = static_cast<uint8_t *>(pic->data[i]);
        for (unsigned j = 0; j < pic->h[i]; j++) {
            memcpy(pic_data, ycbcr_data, sizeof(*pic_data) * pic->w[i]);
            pic_data += pic->stride[i];
            ycbcr_data += ycbcr[i].stride;
        }
    }
}

void copy_equal_depth_high(VmafPicture *pic, video_input_ycbcr ycbcr, const video_input_info *info)
{
    for (unsigned i = 0; i < 3; i++) {
        const int xdec = i && !(info->pixel_fmt & 1);
        const int ydec = i && !(info->pixel_fmt & 2);
        const uint16_t *ycbcr_data =
            reinterpret_cast<const uint16_t *>(ycbcr[i].data) +
            static_cast<size_t>(info->pic_y >> ydec) * (ycbcr[i].stride / 2) +
            (info->pic_x >> xdec);
        uint16_t *pic_data = static_cast<uint16_t *>(pic->data[i]);
        for (unsigned j = 0; j < pic->h[i]; j++) {
            memcpy(pic_data, ycbcr_data, sizeof(*pic_data) * pic->w[i]);
            pic_data += pic->stride[i] / 2;
            ycbcr_data += ycbcr[i].stride / 2;
        }
    }
}

void copy_shifted_8(VmafPicture *pic, video_input_ycbcr ycbcr, const video_input_info *info,
                    int left_shift)
{
    for (unsigned i = 0; i < 3; i++) {
        const int xdec = i && !(info->pixel_fmt & 1);
        const int ydec = i && !(info->pixel_fmt & 2);
        const uint8_t *ycbcr_data = ycbcr[i].data +
                                    static_cast<size_t>(info->pic_y >> ydec) * ycbcr[i].stride +
                                    (info->pic_x >> xdec);
        uint16_t *pic_data = static_cast<uint16_t *>(pic->data[i]);
        for (unsigned j = 0; j < pic->h[i]; j++) {
            for (unsigned k = 0; k < pic->w[i]; k++)
                pic_data[k] = static_cast<uint16_t>(ycbcr_data[k] << left_shift);
            pic_data += pic->stride[i] / 2;
            ycbcr_data += ycbcr[i].stride;
        }
    }
}

} // namespace

namespace
{

void copy_shifted_high(VmafPicture *pic, video_input_ycbcr ycbcr, const video_input_info *info,
                       int left_shift)
{
    for (unsigned i = 0; i < 3; i++) {
        const int xdec = i && !(info->pixel_fmt & 1);
        const int ydec = i && !(info->pixel_fmt & 2);
        const uint16_t *ycbcr_data =
            reinterpret_cast<const uint16_t *>(ycbcr[i].data) +
            static_cast<size_t>(info->pic_y >> ydec) * (ycbcr[i].stride / 2) +
            (info->pic_x >> xdec);
        uint16_t *pic_data = static_cast<uint16_t *>(pic->data[i]);
        for (unsigned j = 0; j < pic->h[i]; j++) {
            for (unsigned k = 0; k < pic->w[i]; k++)
                pic_data[k] = static_cast<uint16_t>(ycbcr_data[k] << left_shift);
            pic_data += pic->stride[i] / 2;
            ycbcr_data += ycbcr[i].stride / 2;
        }
    }
}

void copy_picture_data(VmafPicture *pic, video_input_ycbcr ycbcr, const video_input_info *info,
                       int depth)
{
    if (info->depth == depth) {
        if (depth == 8) {
            copy_equal_depth_8(pic, ycbcr, info);
        } else {
            copy_equal_depth_high(pic, ycbcr, info);
        }
        return;
    }
    if (depth <= 8) {
        return;
    }
    const int left_shift = depth - info->depth;
    if (info->depth == 8) {
        copy_shifted_8(pic, ycbcr, info, left_shift);
    } else {
        copy_shifted_high(pic, ycbcr, info, left_shift);
    }
}

[[nodiscard]] int finish_unread_picture(VmafPicture *pic, int fetch_ret)
{
    const int err_unref = vmaf_picture_unref(pic);
    if (err_unref)
        (void)fprintf(stderr, "\nproblem during vmaf_picture_unref (unread)\n");
    return fetch_ret == 0 ? 1 : -1;
}

} // namespace

namespace
{

[[nodiscard]] int fetch_picture(VmafContext *vmaf, video_input *vid, VmafPicture *pic, int depth)
{
    int ret;
    video_input_info info;

    video_input_get_info(vid, &info);

    ret = vmaf_fetch_preallocated_picture(vmaf, pic);
    if (ret) {
        (void)fprintf(stderr, "problem fetching picture from pool.\n");
        return -1;
    }

#ifdef USE_DIRECT_READ
    (void)depth;
    ret = video_input_fetch_into_vmaf_picture(vid, pic);
    if (ret < 1)
        return finish_unread_picture(pic, ret);
#else
    video_input_ycbcr ycbcr;
    ret = video_input_fetch_frame(vid, ycbcr, nullptr);
    if (ret < 1)
        return finish_unread_picture(pic, ret);
    copy_picture_data(pic, ycbcr, &info, depth);
#endif
    return 0;
}

/* Three parallel model-tracking arrays. CliRunState owns this aggregate and
 * releases it after the context and CLI settings, preserving the historical
 * cleanup order without a control-flow jump. */
struct ModelArrays {
    VmafModel **model;
    VmafModelCollection **collection;
    const char **collection_label;
    unsigned model_cnt;
    unsigned collection_cnt;
};

[[nodiscard]] int allocate_model_arrays(ModelArrays *arrays, unsigned cnt)
{
    arrays->model_cnt = cnt;
    if (cnt == 0)
        return 0;
    arrays->model = static_cast<VmafModel **>(calloc(cnt, sizeof(*arrays->model)));
    if (!arrays->model)
        return -1;
    arrays->collection =
        static_cast<VmafModelCollection **>(calloc(cnt, sizeof(*arrays->collection)));
    if (!arrays->collection)
        return -1;
    arrays->collection_label =
        static_cast<const char **>(calloc(cnt, sizeof(*arrays->collection_label)));
    return arrays->collection_label ? 0 : -1;
}

} // namespace

namespace
{

void destroy_model_arrays(ModelArrays *arrays)
{
    if (arrays->model) {
        for (unsigned i = 0; i < arrays->model_cnt; i++)
            vmaf_model_destroy(arrays->model[i]);
        free(static_cast<void *>(arrays->model));
    }
    if (arrays->collection) {
        for (unsigned i = 0; i < arrays->collection_cnt; i++)
            vmaf_model_collection_destroy(arrays->collection[i]);
        free(static_cast<void *>(arrays->collection));
    }
    free(static_cast<void *>(arrays->collection_label));
}

/* Helper: pick the human-readable label (version preferred over path) for
 * the given model-config entry, used in error messages.
 */
const char *model_label(const CLISettings *c, unsigned i)
{
    return c->model_config[i].version ? c->model_config[i].version : c->model_config[i].path;
}

} // namespace

namespace
{

[[nodiscard]] int overload_model_collection_features(CLISettings *c, unsigned i,
                                                     ModelArrays &arrays, unsigned slot)
{
    for (unsigned j = 0; j < c->model_config[i].overload_cnt; j++) {
        const int err = vmaf_model_collection_feature_overload(
            arrays.model[i], &arrays.collection[slot], c->model_config[i].feature_overload[j].name,
            c->model_config[i].feature_overload[j].opts_dict);
        if (err) {
            (void)fprintf(stderr,
                          "problem overloading feature extractors from model collection: %s\n",
                          model_label(c, i));
            return -1;
        }
    }
    return 0;
}

} // namespace

/* Initialise a model-collection slot for entry `i`. The caller passes the
 * current `*slot` index; on any failure path this helper bumps `*slot`
 * before returning so the caller's cleanup loop unwinds the partially
 * initialised entry. Returns 0 on success.
 */
namespace
{

[[nodiscard]] int load_model_collection_entry(VmafContext *vmaf, CLISettings *c, unsigned i,
                                              ModelArrays &arrays, unsigned w, unsigned h,
                                              enum VmafPixelFormat pix_fmt)
{
    unsigned *slot = &arrays.collection_cnt;
    int err;

    if (c->model_config[i].version) {
        err = vmaf_model_collection_load(&arrays.model[i], &arrays.collection[*slot],
                                         &c->model_config[i].cfg, c->model_config[i].version);
    } else {
        err =
            vmaf_model_collection_load_from_path(&arrays.model[i], &arrays.collection[*slot],
                                                 &c->model_config[i].cfg, c->model_config[i].path);
    }

    if (err) {
        (void)fprintf(stderr, "problem loading model: %s\n", model_label(c, i));
        return -1;
    }

    arrays.collection_label[*slot] = model_label(c, i);

    char err_msg[512] = {0};
    if (vmaf_validate_model_dimensions(arrays.model[i], model_label(c, i), w, h, pix_fmt, err_msg,
                                       sizeof(err_msg))) {
        (void)fprintf(stderr, "error: %s.%s\n", err_msg,
                      c->model_config[i].is_default ?
                          " Pass --model explicitly to use a different model." :
                          "");
        (*slot)++;
        return -EINVAL;
    }

    if (overload_model_collection_features(c, i, arrays, *slot)) {
        (*slot)++;
        return -1;
    }

    err = vmaf_use_features_from_model_collection(vmaf, arrays.collection[*slot]);
    if (err) {
        (void)fprintf(stderr, "problem loading feature extractors from model collection: %s\n",
                      model_label(c, i));
        (*slot)++;
        return -1;
    }

    (*slot)++;
    return 0;
}

} // namespace

/* Load a single model entry from the CLI configuration. Handles the model
 * vs model-collection fallback that the `--model` option's overloaded
 * semantics require.
 */
namespace
{

[[nodiscard]] int load_one_model_entry(VmafContext *vmaf, CLISettings *c, unsigned i,
                                       ModelArrays &arrays, unsigned w, unsigned h,
                                       enum VmafPixelFormat pix_fmt)
{
    int err;

    if (c->model_config[i].version) {
        err =
            vmaf_model_load(&arrays.model[i], &c->model_config[i].cfg, c->model_config[i].version);
    } else {
        err = vmaf_model_load_from_path(&arrays.model[i], &c->model_config[i].cfg,
                                        c->model_config[i].path);
    }

    /* `--model` is overloaded: if a single-model load fails, fall back to
     * loading the same identifier as a model collection.
     */
    if (err) {
        return load_model_collection_entry(vmaf, c, i, arrays, w, h, pix_fmt);
    }

    char err_msg[512] = {0};
    if (vmaf_validate_model_dimensions(arrays.model[i], model_label(c, i), w, h, pix_fmt, err_msg,
                                       sizeof(err_msg))) {
        (void)fprintf(stderr, "error: %s.%s\n", err_msg,
                      c->model_config[i].is_default ?
                          " Pass --model explicitly to use a different model." :
                          "");
        return -EINVAL;
    }

    for (unsigned j = 0; j < c->model_config[i].overload_cnt; j++) {
        err = vmaf_model_feature_overload(arrays.model[i],
                                          c->model_config[i].feature_overload[j].name,
                                          c->model_config[i].feature_overload[j].opts_dict);
        if (err) {
            (void)fprintf(stderr, "problem overloading feature extractors from model: %s\n",
                          model_label(c, i));
            return -1;
        }
    }

    err = vmaf_use_features_from_model(vmaf, arrays.model[i]);
    if (err) {
        (void)fprintf(stderr, "problem loading feature extractors from model: %s\n",
                      model_label(c, i));
        return -1;
    }

    return 0;
}

} // namespace

/* Open both reference and distorted input streams (raw YUV via raw_input_open
 * when --use_yuv is set, otherwise the codec auto-detection path via
 * video_input_open). On success transfers FILE* ownership from *file_ref/dist
 * to the corresponding video_input and zeros the pointers so the cleanup
 * fclose() doesn't double-close. Sets *vid_ref_open / *vid_dist_open to true
 * for cleanup unwinding. Returns 0 on success and -1 on any failure.
 */
namespace
{

[[nodiscard]] int open_input_videos(const CLISettings *c, FILE **file_ref, FILE **file_dist,
                                    video_input *vid_ref, video_input *vid_dist, bool *vid_ref_open,
                                    bool *vid_dist_open)
{
    int err;

    if (c->use_yuv) {
        err = raw_input_open(vid_ref, *file_ref, c->width, c->height, c->pix_fmt, c->bitdepth);
    } else {
        err = video_input_open(vid_ref, *file_ref);
    }
    if (err) {
        /* ADR-0520: --no-reference re-opens the distorted file as the
         * "ref" slot; surface the actually-opened path on failure. */
        const char *const opened_path = c->no_reference ? c->path_dist : c->path_ref;
        (void)fprintf(stderr, "problem with reference file: %s\n", opened_path);
        return -1;
    }
    *vid_ref_open = true;
    *file_ref = nullptr; /* ownership transferred to vid_ref */

    if (c->use_yuv) {
        err = raw_input_open(vid_dist, *file_dist, c->width, c->height, c->pix_fmt, c->bitdepth);
    } else {
        err = video_input_open(vid_dist, *file_dist);
    }
    if (err) {
        (void)fprintf(stderr, "problem with distorted file: %s\n", c->path_dist);
        return -1;
    }
    *vid_dist_open = true;
    *file_dist = nullptr; /* ownership transferred to vid_dist */

    err = validate_videos(vid_ref, vid_dist, c->common_bitdepth);
    if (err) {
        (void)fprintf(stderr, "videos are incompatible, %d %s.\n", err,
                      err == 1 ? "problem" : "problems");
        return -1;
    }

    return 0;
}

} // namespace

namespace
{

struct GpuStates {
#ifdef HAVE_SYCL
    VmafSyclState *sycl_state;
    bool sycl_active;
#endif
#ifdef HAVE_CUDA
    bool cuda_active;
#endif
#ifdef HAVE_HIP
    VmafHipState *hip_state;
    bool hip_active;
#endif
#ifdef HAVE_METAL
    VmafMetalState *metal_state;
    bool metal_active;
#endif
};

[[nodiscard]] bool explicit_backend_requested(const CLISettings *c)
{
    return c->backend && strcmp(c->backend, "auto") != 0 && strcmp(c->backend, "cpu") != 0;
}

} // namespace

namespace
{

#if defined(HAVE_SYCL) || defined(HAVE_CUDA) || defined(HAVE_HIP) || defined(HAVE_METAL)
[[nodiscard]] bool backend_compiled_in(const char *backend)
{
    return
#ifdef HAVE_SYCL
        strcmp(backend, "sycl") == 0 ||
#endif
#ifdef HAVE_CUDA
        strcmp(backend, "cuda") == 0 ||
#endif
#ifdef HAVE_HIP
        strcmp(backend, "hip") == 0 ||
#endif
#ifdef HAVE_METAL
        strcmp(backend, "metal") == 0 ||
#endif
        false;
}
#endif

[[nodiscard]] int validate_requested_backend(const CLISettings *c)
{
    if (!explicit_backend_requested(c))
        return 0;
#if defined(HAVE_SYCL) || defined(HAVE_CUDA) || defined(HAVE_HIP) || defined(HAVE_METAL)
    if (backend_compiled_in(c->backend))
        return 0;
#endif
    (void)fprintf(stderr,
                  "vmaf: --backend %s requested but this libvmaf was built without %s "
                  "support; refusing to silently fall back to CPU (ADR-0498)\n",
                  c->backend, c->backend);
    write_backend_error_json(c->output_path, c->output_fmt, c->backend,
                             "backend not compiled into this libvmaf", 0);
    return VMAF_INIT_GPU_EXPLICIT_FAIL;
}

} // namespace

#ifdef HAVE_SYCL
namespace
{

[[nodiscard]] int init_sycl_backend(VmafContext *vmaf, const CLISettings *c, GpuStates *states)
{
    const VmafSyclConfiguration cfg = {
        .device_index = c->sycl_device >= 0 ? c->sycl_device : 0,
    };
    if ((c->sycl_device < 0 && !c->use_gpumask) || c->no_sycl)
        return 0;
    int err = vmaf_sycl_state_init(&states->sycl_state, cfg);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_sycl_state_init, using CPU\n");
        if (!explicit_backend_requested(c) || strcmp(c->backend, "sycl") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend sycl requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "sycl",
                                 "vmaf_sycl_state_init failed", err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    err = vmaf_sycl_import_state(vmaf, states->sycl_state);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_sycl_import_state\n");
        return -1;
    }
    states->sycl_active = true;
    return 0;
}

} // namespace
#endif

#ifdef HAVE_CUDA
namespace
{

[[nodiscard]] int init_cuda_backend(VmafContext *vmaf, const CLISettings *c, GpuStates *states)
{
    if (!c->use_gpumask || c->no_cuda)
        return 0;
#ifdef HAVE_SYCL
    if (states->sycl_active)
        return 0;
#endif
    VmafCudaState *cuda_state;
    const VmafCudaConfiguration cfg = {0};
    int err = vmaf_cuda_state_init(&cuda_state, cfg);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_cuda_state_init, using CPU\n");
        if (!explicit_backend_requested(c) || strcmp(c->backend, "cuda") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend cuda requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "cuda",
                                 "vmaf_cuda_state_init failed", err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    err = vmaf_cuda_import_state(vmaf, cuda_state);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_cuda_import_state\n");
        return -1;
    }
    states->cuda_active = true;
    return 0;
}

} // namespace
#endif

#ifdef HAVE_HIP
namespace
{

[[nodiscard]] int init_hip_backend(VmafContext *vmaf, const CLISettings *c, GpuStates *states)
{
    const VmafHipConfiguration cfg = {.device_index = c->hip_device, .flags = 0};
    if (c->hip_device < 0 || c->no_hip)
        return 0;
    int err = vmaf_hip_state_init(&states->hip_state, cfg);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_hip_state_init (%d), using CPU\n", err);
        if (!explicit_backend_requested(c) || strcmp(c->backend, "hip") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend hip requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "hip", "vmaf_hip_state_init failed",
                                 err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    err = vmaf_hip_import_state(vmaf, states->hip_state);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_hip_import_state\n");
        return -1;
    }
    states->hip_active = true;
    return 0;
}

} // namespace
#endif

#ifdef HAVE_METAL
namespace
{

[[nodiscard]] int init_metal_backend(VmafContext *vmaf, const CLISettings *c, GpuStates *states)
{
    const VmafMetalConfiguration cfg = {.device_index = c->metal_device, .flags = 0};
    if (c->metal_device < 0 || c->no_metal)
        return 0;
    int err = vmaf_metal_state_init(&states->metal_state, cfg);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_metal_state_init (%d), using CPU\n", err);
        if (!explicit_backend_requested(c) || strcmp(c->backend, "metal") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend metal requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "metal",
                                 "vmaf_metal_state_init failed", err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    err = vmaf_metal_import_state(vmaf, states->metal_state);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_metal_import_state\n");
        return -1;
    }
    states->metal_active = true;
    return 0;
}

} // namespace
#endif

namespace
{

[[nodiscard]] int init_gpu_backends(VmafContext *vmaf, const CLISettings *c, GpuStates *states)
{
    const int requested_err = validate_requested_backend(c);
    if (requested_err)
        return requested_err;
#ifdef HAVE_SYCL
    const int sycl_err = init_sycl_backend(vmaf, c, states);
    if (sycl_err)
        return sycl_err;
#endif
#ifdef HAVE_CUDA
    const int cuda_err = init_cuda_backend(vmaf, c, states);
    if (cuda_err)
        return cuda_err;
#endif
#ifdef HAVE_HIP
    const int hip_err = init_hip_backend(vmaf, c, states);
    if (hip_err)
        return hip_err;
#endif
#ifdef HAVE_METAL
    const int metal_err = init_metal_backend(vmaf, c, states);
    if (metal_err)
        return metal_err;
#endif
    (void)vmaf;
    (void)states;
    return 0;
}

} // namespace

/* ADR-0543 (extends ADR-0498): a feature whose name ends in ``_cuda``
 * / ``_sycl`` / ``_hip`` / ``_metal`` is a GPU-pinned
 * variant. Asking for ``--feature integer_motion_hip`` against a
 * libvmaf build without HIP — or with HIP compiled in but no device
 * available — silently registers the CPU twin and produces scores
 * that look identical to the explicit-backend invocation, but were
 * actually computed on the CPU. That defeats the entire point of the
 * explicit-backend gate.
 *
 * This helper hard-fails any GPU-pinned feature name when the matching
 * backend isn't compiled into this binary, OR is compiled in but the
 * matching ``--<backend>_device`` / ``--backend <name>`` wasn't
 * requested (so no state_init was attempted) or the state_init failed
 * (in which case init_gpu_backends has already errored out earlier
 * and we never reach here). Returns 0 on success, -1 on mismatch.
 *
 * Returns the backend keyword via *requested_backend_out (caller-
 * owned; points into a static string table) so the caller can include
 * the keyword in the error JSON. */
namespace
{

[[nodiscard]] int feature_backend_suffix(const char *feature_name, const char **backend_out)
{
    if (!feature_name || !backend_out)
        return 0;
    static const struct {
        const char *suffix;
        const char *backend;
    } table[] = {
        {.suffix = "_cuda", .backend = "cuda"},
        {.suffix = "_sycl", .backend = "sycl"},
        {.suffix = "_hip", .backend = "hip"},
        {.suffix = "_metal", .backend = "metal"},
    };
    const size_t nlen = strlen(feature_name);
    for (const auto &item : table) {
        const size_t slen = strlen(item.suffix);
        if (nlen > slen && strcmp(feature_name + nlen - slen, item.suffix) == 0) {
            *backend_out = item.backend;
            return 1;
        }
    }
    return 0;
}

} // namespace

/* Returns 1 when the named backend is active in this run (state_init
 * succeeded and the matching ``--<backend>_device`` was requested),
 * 0 otherwise. The active flags live in main() so this helper accepts
 * each as a parameter. */
namespace
{

#if defined(HAVE_SYCL) || defined(HAVE_CUDA) || defined(HAVE_HIP) || defined(HAVE_METAL)
[[nodiscard]] int backend_active(const char *backend, const GpuStates *states)
{
    if (!strcmp(backend, "sycl")) {
        return
#ifdef HAVE_SYCL
            states->sycl_active ? 1 : 0;
#else
            0;
#endif
    }
    if (!strcmp(backend, "cuda")) {
        return
#ifdef HAVE_CUDA
            states->cuda_active ? 1 : 0;
#else
            0;
#endif
    }
    if (!strcmp(backend, "hip")) {
        return
#ifdef HAVE_HIP
            states->hip_active ? 1 : 0;
#else
            0;
#endif
    }
    if (!strcmp(backend, "metal")) {
        return
#ifdef HAVE_METAL
            states->metal_active ? 1 : 0;
#else
            0;
#endif
    }
    (void)states;
    return 0;
}
#endif

} // namespace

/* Translate the textual --tiny-device flag (cpu / cuda / openvino /
 * coreml / coreml-ane / coreml-gpu / coreml-cpu / openvino-npu /
 * openvino-cpu / openvino-gpu / rocm) into the corresponding
 * VmafDnnDevice enum. The coreml-* keywords pin the CoreML EP to a
 * single MLComputeUnits value (see ADR-0365); plain `coreml` lets
 * CoreML auto-route across compute units. The openvino-* keywords pin
 * the OpenVINO EP to a single device type with no fallback (see
 * Research-0031); plain `openvino` keeps the GPU→CPU fallback chain.
 * Unknown values fall back to VMAF_DNN_DEVICE_AUTO so the runtime
 * picks a default.
 */
namespace
{

[[nodiscard]] VmafDnnDevice resolve_tiny_device(const char *name)
{
    if (!name)
        return VMAF_DNN_DEVICE_AUTO;
    using sv = std::string_view;
    const sv n{name};
    if (n == "cpu")
        return VMAF_DNN_DEVICE_CPU;
    if (n == "cuda")
        return VMAF_DNN_DEVICE_CUDA;
    if (n == "openvino")
        return VMAF_DNN_DEVICE_OPENVINO;
    if (n == "coreml")
        return VMAF_DNN_DEVICE_COREML;
    if (n == "coreml-ane")
        return VMAF_DNN_DEVICE_COREML_ANE;
    if (n == "coreml-gpu")
        return VMAF_DNN_DEVICE_COREML_GPU;
    if (n == "coreml-cpu")
        return VMAF_DNN_DEVICE_COREML_CPU;
    if (n == "openvino-npu")
        return VMAF_DNN_DEVICE_OPENVINO_NPU;
    if (n == "openvino-cpu")
        return VMAF_DNN_DEVICE_OPENVINO_CPU;
    if (n == "openvino-gpu")
        return VMAF_DNN_DEVICE_OPENVINO_GPU;
    if (n == "rocm")
        return VMAF_DNN_DEVICE_ROCM;
    return VMAF_DNN_DEVICE_AUTO;
}

} // namespace

/* Configure the tiny-AI (DNN) model on the VMAF context when --tiny-model
 * is passed. Performs the optional Sigstore-bundle verification (T6-9 /
 * ADR-0211) before opening the model so a signature failure short-circuits
 * load and never touches ORT. Returns 0 on success and -1 on any failure.
 */
namespace
{

[[nodiscard]] int apply_tiny_resize(VmafContext *const vmaf, const char *const tiny_resize)
{
    if (!tiny_resize)
        return 0;
    VmafDnnResizeMode mode = VMAF_DNN_RESIZE_DISABLED;
    using sv = std::string_view;
    const sv rsz{tiny_resize};
    if (rsz == "bilinear") {
        mode = VMAF_DNN_RESIZE_BILINEAR;
    } else if (rsz == "nearest") {
        mode = VMAF_DNN_RESIZE_NEAREST;
    } else if (rsz == "bicubic") {
        mode = VMAF_DNN_RESIZE_BICUBIC;
    } else if (rsz == "disabled") {
        mode = VMAF_DNN_RESIZE_DISABLED;
    }
    const int rerr = vmaf_dnn_set_resize_mode(vmaf, mode);
    if (rerr != 0) {
        (void)fprintf(stderr, "--tiny-resize: vmaf_dnn_set_resize_mode failed (errno %d)\n", -rerr);
        return -1;
    }
    return 0;
}

} // namespace

namespace
{

[[nodiscard]] int apply_tiny_codec(VmafContext *const vmaf, const char *const codec,
                                   const char *const preset, const int crf_setting)
{
    if (!codec && !preset && crf_setting < 0)
        return 0;
    const int crf = crf_setting >= 0 ? crf_setting : 0;
    const int cerr = vmaf_dnn_set_codec_context(vmaf, codec, preset, crf);
    if (cerr == -ENOENT) {
        (void)fprintf(stderr,
                      "--tiny-codec '%s' not found in model encoder_vocab; "
                      "use one of the names listed by --help.\n",
                      codec ? codec : "(null)");
        return -1;
    }
    if (cerr == -ENOTSUP) {
        (void)fprintf(stderr, "--tiny-codec / --tiny-preset / --tiny-crf require a "
                              "codec-aware tiny model (loaded model has no codec block).\n");
        return -1;
    }
    if (cerr != 0) {
        (void)fprintf(stderr, "vmaf_dnn_set_codec_context failed (errno %d)\n", -cerr);
        return -1;
    }
    return 0;
}

} // namespace

namespace
{

[[nodiscard]] int configure_tiny_model(VmafContext *const vmaf, const CLISettings *const c)
{
    if (!c->tiny_model_path)
        return 0;

    if (!vmaf_dnn_available()) {
        (void)fprintf(stderr,
                      "--tiny-model requested (%s) but libvmaf was built "
                      "without DNN support (-Denable_dnn=disabled).\n",
                      c->tiny_model_path);
        return -1;
    }
    if (c->tiny_model_verify) {
        const int verr = vmaf_dnn_verify_signature(c->tiny_model_path, nullptr);
        if (verr != 0) {
            (void)fprintf(stderr,
                          "--tiny-model-verify: signature verification "
                          "failed for %s (errno %d)\n",
                          c->tiny_model_path, -verr);
            return -1;
        }
    }
    const VmafDnnConfig dnn_cfg = {
        .device = resolve_tiny_device(c->tiny_device),
        .device_index = 0,
        .threads = c->tiny_threads,
        .fp16_io = c->tiny_fp16,
    };
    const int err = vmaf_use_tiny_model(vmaf, c->tiny_model_path, &dnn_cfg);
    if (err) {
        (void)fprintf(stderr, "problem loading tiny model %s: %d\n", c->tiny_model_path, err);
        return -1;
    }

    if (apply_tiny_resize(vmaf, c->tiny_resize) != 0)
        return -1;

    return apply_tiny_codec(vmaf, c->tiny_codec, c->tiny_preset, c->tiny_crf);
}

} // namespace

/* Skip the first `c->frame_skip_ref` ref frames and `c->frame_skip_dist` dist
 * frames, releasing each one back to the picture pool. fetch_picture() reserves
 * a slot from the preallocated pool, and skipped frames are never handed to
 * vmaf_read_pictures() to release them; without unref the pool is exhausted
 * after N skips and the next fetch blocks indefinitely.
 */
namespace
{

void skip_initial_frames(VmafContext *vmaf, video_input *vid_ref, video_input *vid_dist,
                         const CLISettings *c, int common_bitdepth)
{
    VmafPicture pic_ref_skip;
    VmafPicture pic_dist_skip;

    for (unsigned i = 0; i < c->frame_skip_ref; i++) {
        if (fetch_picture(vmaf, vid_ref, &pic_ref_skip, common_bitdepth))
            break;
        if (vmaf_picture_unref(&pic_ref_skip))
            (void)fprintf(stderr, "\nproblem during vmaf_picture_unref (skip ref)\n");
    }

    for (unsigned i = 0; i < c->frame_skip_dist; i++) {
        if (fetch_picture(vmaf, vid_dist, &pic_dist_skip, common_bitdepth))
            break;
        if (vmaf_picture_unref(&pic_dist_skip))
            (void)fprintf(stderr, "\nproblem during vmaf_picture_unref (skip dist)\n");
    }
}

/* Wall-clock timer for the FPS spinner in run_frame_loop. clock() /
 * CLOCKS_PER_SEC measures aggregate CPU process time — under a multi-threaded
 * run every worker thread contributes, so the FPS reading over-counts by up to
 * n_threads. CLOCK_MONOTONIC / QueryPerformanceCounter give true wall time. */
#ifdef _WIN32
double wall_time_s()
{
    /* The performance-counter frequency is fixed at boot, so query it once and
     * cache it (static, zero-initialised) instead of every FPS update. */
    static LARGE_INTEGER freq;
    LARGE_INTEGER cnt = {0};
    if (!freq.QuadPart)
        (void)QueryPerformanceFrequency(&freq);
    (void)QueryPerformanceCounter(&cnt);
    return freq.QuadPart ? static_cast<double>(cnt.QuadPart) / static_cast<double>(freq.QuadPart) :
                           0.0;
}
#else
double wall_time_s()
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
#endif

} // namespace

#ifdef _WIN32
/* Some older Windows SDK headers predate the VT console mode flag. */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

/*
 * Netflix/vmaf#743: the CLI writes the UTF-8 braille spinner and a `\033[K`
 * erase-to-EOL straight to stderr with fprintf, but nothing in the tree ever
 * set the console output code page or enabled VT processing — so under the
 * default OEM/ANSI code pages the spinner rendered as mojibake and legacy
 * conhost printed the CSI sequence literally, on every frame of every run.
 *
 * Switch the console to UTF-8 and enable VT for the life of the process, and
 * restore whatever was there on the way out. The guard has static storage so
 * cli_parse()'s direct exit paths restore it too. Whatever the console refuses is reflected back
 * through console_progress_style(), which then falls back to the ASCII table
 * and space padding. No effect on POSIX: the whole class is #ifdef'd out.
 */
namespace
{

class WindowsConsoleGuard
{
  public:
    WindowsConsoleGuard()
    {
        prev_code_page_ = GetConsoleOutputCP();
        if (prev_code_page_ != 0 && prev_code_page_ != CP_UTF8)
            code_page_changed_ = SetConsoleOutputCP(CP_UTF8) != 0;

        const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &prev_mode_) != 0) {
            const DWORD wanted = prev_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (wanted != prev_mode_)
                mode_changed_ = SetConsoleMode(h, wanted) != 0;
        }
    }

    WindowsConsoleGuard(const WindowsConsoleGuard &) = delete;
    WindowsConsoleGuard &operator=(const WindowsConsoleGuard &) = delete;

    ~WindowsConsoleGuard()
    {
        if (code_page_changed_)
            (void)SetConsoleOutputCP(prev_code_page_);
        if (mode_changed_) {
            const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
            if (h != INVALID_HANDLE_VALUE)
                (void)SetConsoleMode(h, prev_mode_);
        }
    }

  private:
    UINT prev_code_page_ = 0;
    DWORD prev_mode_ = 0;
    bool code_page_changed_ = false;
    bool mode_changed_ = false;
};

} // namespace

#endif /* _WIN32 */

/* Glyph table + erase-to-EOL sequence for the interactive progress line. */
namespace
{

struct ProgressStyle {
    const char *const *table;
    unsigned length;
    const char *erase_eol;
};

/*
 * Resolve the progress-line style from the console's ACTUAL capabilities.
 * On POSIX this is unconditionally the braille table plus "\033[K", so the
 * emitted bytes are identical to the pre-Netflix/vmaf#743 behaviour.
 */
#ifdef _WIN32
unsigned console_output_code_page()
{
    const UINT cp = GetConsoleOutputCP();
    return (cp != 0) ? static_cast<unsigned>(cp) : 0u;
}

int console_vt_enabled()
{
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    /* A redirected stderr has no console mode; raw bytes reach the file or
     * pipe unmodified, so the CSI sequence is fine there. */
    if (h == INVALID_HANDLE_VALUE || GetConsoleMode(h, &mode) == 0)
        return 1;
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#else
unsigned console_output_code_page()
{
    return SPINNER_CODEPAGE_UTF8;
}

int console_vt_enabled()
{
    return 1;
}
#endif

} // namespace

/*
 * Emit one interactive progress line. Netflix/vmaf#743: the glyph table and
 * the erase-to-EOL sequence come from the console's actual capabilities, so a
 * Windows console that refuses UTF-8 or VT gets ASCII plus space padding
 * instead of mojibake and a literal erase sequence. On POSIX the style is
 * always the braille table plus the CSI erase, i.e. the emitted bytes are
 * identical to the previous unconditional form.
 */
namespace
{

void emit_progress_line(const ProgressStyle &style, unsigned picture_index, float fps)
{
    (void)fprintf(stderr, "\r%u frame%s %s %.2f FPS%s", picture_index + 1,
                  picture_index ? "s" : " ", style.table[picture_index % style.length], fps,
                  style.erase_eol);
    (void)fflush(stderr);
}

ProgressStyle console_progress_style()
{
    const unsigned code_page = console_output_code_page();
    const int vt_enabled = console_vt_enabled();

    ProgressStyle style;
    style.length = 0;
    style.table = spinner_table_for_codepage(code_page, &style.length);
    style.erase_eol = spinner_erase_eol(vt_enabled);
    return style;
}

} // namespace

/* What a pair of fetch_picture() results means for the frame loop. */
namespace
{

enum FrameFetchOutcome : std::uint8_t {
    FRAME_FETCH_OK,    /* both pictures read; keep going */
    FRAME_FETCH_END,   /* clean end of stream on one or both sides */
    FRAME_FETCH_ERROR, /* at least one side failed to read */
};

/* Classify one pair of fetch_picture() results and emit the matching
 * diagnostic. fetch_picture() returns 0 for a usable picture, 1 at end of
 * stream and -1 on a read error (see finish_unread_picture).
 *
 * The error test comes FIRST, and that ordering is the whole point. `ret1 &&
 * ret2` is true whenever both sides are non-zero, which includes both sides
 * returning -1 -- so testing it first classified two failed reads as a clean
 * end of stream, printing nothing and leaving the exit status at 0. A pair of
 * truncated files then produced a full report over the frames that happened
 * to arrive. See ADR-1262 and core/tools/test/test_vmaf_read_error_exit.sh. */
FrameFetchOutcome classify_frame_fetch(int ret1, int ret2, const CLISettings *c)
{
    if (ret1 < 0 || ret2 < 0) {
        (void)fprintf(stderr, "\nproblem while reading pictures\n");
        return FRAME_FETCH_ERROR;
    }
    if (ret1 && ret2)
        return FRAME_FETCH_END;
    if (ret1) {
        (void)fprintf(stderr, "\n\"%s\" ended before \"%s\".\n", c->path_ref, c->path_dist);
        return FRAME_FETCH_END;
    }
    if (ret2) {
        (void)fprintf(stderr, "\n\"%s\" ended before \"%s\".\n", c->path_dist, c->path_ref);
        return FRAME_FETCH_END;
    }
    return FRAME_FETCH_OK;
}

} // namespace

/* Outcome of the per-frame fetch + process loop.
 *
 * `frames` is the number of frames successfully consumed (the post-increment
 * `picture_index` value the pooling path uses to compute `picture_index - 1`).
 * `exit_code` is 0 when the loop stopped for a benign reason -- end of either
 * stream, or c->frame_cnt reached -- and non-zero when it stopped because a
 * read failed. Reporting only the count, as this loop used to, gave main() no
 * way to tell the two apart. */
namespace
{

struct FrameLoopResult {
    unsigned frames;
    int exit_code;
};

} // namespace

namespace
{

void release_unpaired_pictures(int ret1, int ret2, VmafPicture *pic_ref, VmafPicture *pic_dist)
{
    if (!ret1) {
        const int err_unref = vmaf_picture_unref(pic_ref);
        if (err_unref)
            (void)fprintf(stderr, "\nproblem during vmaf_picture_unref\n");
    }
    if (!ret2) {
        const int err_unref = vmaf_picture_unref(pic_dist);
        if (err_unref)
            (void)fprintf(stderr, "\nproblem during vmaf_picture_unref\n");
    }
}

} // namespace

/* Drive the main per-frame fetch + process loop. Stops at EOF on either side,
 * on read errors, or when c->frame_cnt is reached; see FrameLoopResult for how
 * the caller tells those apart.
 */
namespace
{

FrameLoopResult run_frame_loop(VmafContext *vmaf, video_input *vid_ref, video_input *vid_dist,
                               const CLISettings *c, int common_bitdepth, int istty)
{
    float fps = 0.;
    const double t0 = wall_time_s();
    const ProgressStyle progress_style = console_progress_style();
    int exit_code = 0;
    unsigned picture_index;
    for (picture_index = 0;; picture_index++) {

        if (c->frame_cnt && picture_index >= c->frame_cnt)
            break;

        VmafPicture pic_ref;
        VmafPicture pic_dist;
        const int ret1 = fetch_picture(vmaf, vid_ref, &pic_ref, common_bitdepth);
        const int ret2 = fetch_picture(vmaf, vid_dist, &pic_dist, common_bitdepth);

        if (ret1 || ret2)
            release_unpaired_pictures(ret1, ret2, &pic_ref, &pic_dist);

        const FrameFetchOutcome outcome = classify_frame_fetch(ret1, ret2, c);
        if (outcome == FRAME_FETCH_ERROR) {
            exit_code = VMAF_EXIT_INPUT_READ_ERROR;
            break;
        }
        if (outcome == FRAME_FETCH_END)
            break;

        if (istty && !c->quiet) {
            if (picture_index > 0 && !(picture_index % 10)) {
                fps = static_cast<float>((picture_index + 1) / (wall_time_s() - t0));
            }

            emit_progress_line(progress_style, picture_index, fps);
        }

        const int err = vmaf_read_pictures(vmaf, &pic_ref, &pic_dist, picture_index);
        if (err) {
            (void)fprintf(stderr, "\nproblem reading pictures\n");
            /* Handing the pictures to the library failed, so this frame never
             * entered the score. Same reasoning as a failed read: do not let
             * the run report success over the frames that came before. */
            exit_code = err;
            break;
        }
    }
    if (istty && !c->quiet)
        (void)fprintf(stderr, "\n");

    return {.frames = picture_index, .exit_code = exit_code};
}

} // namespace

/* Compute and report pooled VMAF scores for all loaded models and model
 * collections. Called only when c->no_prediction is false. Returns 0 on
 * success and non-zero on the first per-model scoring failure.
 */
namespace
{

[[nodiscard]] int report_pooled_scores(VmafContext *vmaf, const CLISettings *c,
                                       const ModelArrays &arrays, unsigned picture_index, int istty)
{
    for (unsigned i = 0; i < c->model_cnt; i++) {
        double vmaf_score;
        const int err = vmaf_score_pooled(vmaf, arrays.model[i], VMAF_POOL_METHOD_MEAN, &vmaf_score,
                                          0, picture_index - 1);
        if (err) {
            (void)fprintf(stderr, "problem generating pooled VMAF score\n");
            return -1;
        }

        if (istty && (!c->quiet || !c->output_path)) {
            (void)fprintf(stderr, "%s: ",
                          c->model_config[i].version ? c->model_config[i].version :
                                                       c->model_config[i].path);
            (void)fprintf(stderr, c->precision_fmt, vmaf_score);
            (void)fprintf(stderr, "\n");
        }
    }

    for (unsigned i = 0; i < arrays.collection_cnt; i++) {
        VmafModelCollectionScore score = {.type = static_cast<VmafModelCollectionScoreType>(0),
                                          .bootstrap = {}};
        const int err = vmaf_score_pooled_model_collection(
            vmaf, arrays.collection[i], VMAF_POOL_METHOD_MEAN, &score, 0, picture_index - 1);
        if (err) {
            (void)fprintf(stderr, "problem generating pooled VMAF score\n");
            return -1;
        }

        /* VmafModelCollectionScoreType has only BOOTSTRAP as a printable
         * variant (UNKNOWN is a no-op), so a plain if is clearer than a
         * two-label switch. */
        if (score.type == VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP && istty &&
            (!c->quiet || !c->output_path)) {
            (void)fprintf(stderr, "%s: ", arrays.collection_label[i]);
            (void)fprintf(stderr, c->precision_fmt, score.bootstrap.bagging_score);
            (void)fprintf(stderr, ", ci.p95: [");
            (void)fprintf(stderr, c->precision_fmt, score.bootstrap.ci.p95.lo);
            (void)fprintf(stderr, ", ");
            (void)fprintf(stderr, c->precision_fmt, score.bootstrap.ci.p95.hi);
            (void)fprintf(stderr, "], stddev: ");
            (void)fprintf(stderr, c->precision_fmt, score.bootstrap.stddev);
            (void)fprintf(stderr, "\n");
        }
    }

    return 0;
}

} // namespace

/* ADR-0498 / Bug #v2-E: amend the JSON output file with a top-level
 * ``"backend_used": "NAME"`` key so downstream consumers (CI gates,
 * MCP probes per PR #1251) can confirm which backend actually ran.
 * Implemented as a textual edit on the closing ``}`` to avoid pulling
 * a JSON parser into the CLI; the writer always emits a single
 * top-level object so the brace is at the file tail.
 *
 * No-op when output_path is NULL or format isn't JSON.
 */
namespace
{

void amend_json_with_backend_used(const char *output_path, enum VmafOutputFormat fmt,
                                  const char *backend_used)
{
    if (!output_path || !backend_used)
        return;
    if (fmt != VMAF_OUTPUT_FORMAT_JSON)
        return;

    FILE *fp = vmaf_fopen_utf8(output_path, "rb+");
    if (!fp)
        return;
    if (fseek(fp, 0, SEEK_END) != 0) {
        (void)fclose(fp);
        return;
    }
    const long size = ftell(fp);
    if (size <= 1) {
        (void)fclose(fp);
        return;
    }
    /* Walk backwards over trailing whitespace + the final '}'. */
    long pos = size - 1;
    while (pos > 0) {
        if (fseek(fp, pos, SEEK_SET) != 0) {
            (void)fclose(fp);
            return;
        }
        const int ch = fgetc(fp);
        if (ch == EOF) {
            (void)fclose(fp);
            return;
        }
        if (ch == '}')
            break;
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            (void)fclose(fp);
            return;
        }
        pos--;
    }
    if (fseek(fp, pos, SEEK_SET) != 0) {
        (void)fclose(fp);
        return;
    }
    (void)fprintf(fp, ", \"backend_used\": \"%s\"}\n", backend_used);
    (void)fclose(fp);
}

} // namespace

namespace
{

struct CliRunState {
    CLISettings c = {};
    FILE *file_ref = nullptr;
    FILE *file_dist = nullptr;
    bool vid_ref_open = false;
    bool vid_dist_open = false;
    video_input vid_ref = {};
    video_input vid_dist = {};
    VmafContext *vmaf = nullptr;
    ModelArrays arrays = {};
    GpuStates gpu = {};
    int common_bitdepth = 0;
    VmafPictureConfiguration pic_cfg = {};
};

void cleanup_gpu_states(GpuStates *states)
{
#ifdef HAVE_SYCL
    if (states->sycl_active)
        vmaf_sycl_state_free(&states->sycl_state);
#endif
#ifdef HAVE_HIP
    if (states->hip_state)
        vmaf_hip_state_free(&states->hip_state);
#endif
#ifdef HAVE_METAL
    if (states->metal_state)
        vmaf_metal_state_free(&states->metal_state);
#endif
    (void)states;
}

void cleanup_cli_run_state(CliRunState *state)
{
    if (state->vmaf)
        vmaf_close(state->vmaf);
    cleanup_gpu_states(&state->gpu);
    if (state->vid_dist_open)
        video_input_close(&state->vid_dist);
    if (state->vid_ref_open)
        video_input_close(&state->vid_ref);
    if (state->file_dist)
        (void)fclose(state->file_dist);
    if (state->file_ref)
        (void)fclose(state->file_ref);
    cli_free(&state->c);
    destroy_model_arrays(&state->arrays);
}

} // namespace

namespace
{

class CliRunGuard
{
  public:
    explicit CliRunGuard(CliRunState *state) : state_(state)
    {
    }
    CliRunGuard(const CliRunGuard &) = delete;
    CliRunGuard &operator=(const CliRunGuard &) = delete;
    ~CliRunGuard()
    {
        cleanup_cli_run_state(state_);
    }

  private:
    CliRunState *state_;
};

void print_cli_banner(const CLISettings *c, int istty)
{
    if (!istty || c->quiet)
        return;
    if (!c->vmafx_mode || c->netflix_compat) {
        (void)fprintf(stderr, "VMAF version %s\n", vmaf_version());
    } else if (c->precision_max) {
        (void)fprintf(stderr, "VMAFX version %s (precision=max)\n", vmaf_version());
    } else {
        (void)fprintf(stderr, "VMAFX version %s\n", vmaf_version());
    }
}

[[nodiscard]] int open_cli_inputs(CliRunState *state)
{
    const char *const ref_path = state->c.no_reference ? state->c.path_dist : state->c.path_ref;
    state->file_ref = vmaf_fopen_utf8(ref_path, "rb");
    if (!state->file_ref) {
        (void)fprintf(stderr, "could not open file: %s\n", ref_path);
        return -1;
    }
    state->file_dist = vmaf_fopen_utf8(state->c.path_dist, "rb");
    if (!state->file_dist) {
        (void)fprintf(stderr, "could not open file: %s\n", state->c.path_dist);
        return -1;
    }
    return open_input_videos(&state->c, &state->file_ref, &state->file_dist, &state->vid_ref,
                             &state->vid_dist, &state->vid_ref_open, &state->vid_dist_open);
}

} // namespace

namespace
{

[[nodiscard]] int derive_common_bitdepth(CliRunState *state)
{
    if (state->c.use_yuv)
        return static_cast<int>(state->c.bitdepth);
    video_input_info ref_info;
    video_input_info dist_info;
    video_input_get_info(&state->vid_ref, &ref_info);
    video_input_get_info(&state->vid_dist, &dist_info);
    return ref_info.depth > dist_info.depth ? ref_info.depth : dist_info.depth;
}

[[nodiscard]] int init_cli_context(CliRunState *state)
{
    state->common_bitdepth = derive_common_bitdepth(state);
    const VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = state->c.thread_cnt,
        .n_subsample = state->c.subsample,
        .cpumask = state->c.cpumask,
        .gpumask = state->c.gpumask,
    };
    const int err = vmaf_init(&state->vmaf, cfg);
    if (err)
        (void)fprintf(stderr, "problem initializing VMAF context\n");
    return err ? -1 : 0;
}

[[nodiscard]] int init_cli_backends(CliRunState *state)
{
    const int err = init_gpu_backends(state->vmaf, &state->c, &state->gpu);
    if (err == VMAF_INIT_GPU_EXPLICIT_FAIL)
        return VMAF_EXIT_BACKEND_INIT_FAILED;
    return err ? -1 : 0;
}

} // namespace

namespace
{

[[nodiscard]] int preallocate_cli_pictures(CliRunState *state, int istty)
{
    video_input_info info;
    video_input_get_info(&state->vid_ref, &info);
    state->pic_cfg = {
        .pic_params =
            {
                .w = static_cast<unsigned>(info.pic_w),
                .h = static_cast<unsigned>(info.pic_h),
                .bpc = static_cast<unsigned>(state->common_bitdepth),
                .pix_fmt = pix_fmt_map(info.pixel_fmt),
            },
        .pic_cnt = 2 * (state->c.thread_cnt + 1) + 1,
    };
    const int err = vmaf_preallocate_pictures(state->vmaf, state->pic_cfg);
    if (err) {
        (void)fprintf(stderr, "problem during vmaf_preallocate_pictures\n");
        return -1;
    }
    if (istty && !state->c.quiet) {
        (void)fprintf(stderr, "picture pool: %u pictures pre-allocated\n", state->pic_cfg.pic_cnt);
    }
    return 0;
}

[[nodiscard]] int load_cli_models(CliRunState *state)
{
    if (allocate_model_arrays(&state->arrays, state->c.model_cnt))
        return -1;
    for (unsigned i = 0; i < state->c.model_cnt; i++) {
        const int err = load_one_model_entry(
            state->vmaf, &state->c, i, state->arrays, state->pic_cfg.pic_params.w,
            state->pic_cfg.pic_params.h, state->pic_cfg.pic_params.pix_fmt);
        if (err)
            return -EINVAL;
    }
    return 0;
}

} // namespace

namespace
{

[[nodiscard]] int reject_inactive_feature_backend(const CliRunState *state,
                                                  const char *feature_name,
                                                  const char *requested_backend)
{
    (void)fprintf(stderr,
                  "vmaf: --feature %s pinned to %s backend but %s is not active in this "
                  "run; refusing to silently fall back to CPU (ADR-0498)\n",
                  feature_name, requested_backend, requested_backend);
    write_backend_error_json(state->c.output_path, state->c.output_fmt, requested_backend,
                             "feature pinned to inactive backend", 0);
    return VMAF_EXIT_BACKEND_INIT_FAILED;
}

} // namespace

namespace
{

[[nodiscard]] int register_cli_feature(CliRunState *state, unsigned index)
{
    const auto &feature = state->c.feature_cfg[index];
    char dimension_error[256] = {0};
    if (vmaf_validate_feature_dimensions(
            feature.name, state->pic_cfg.pic_params.w, state->pic_cfg.pic_params.h,
            state->pic_cfg.pic_params.pix_fmt, dimension_error, sizeof(dimension_error))) {
        (void)fprintf(stderr, "error: feature '%s' %s.\n", feature.name, dimension_error);
        return -EINVAL;
    }
    const char *requested_backend = nullptr;
    if (feature_backend_suffix(feature.name, &requested_backend)) {
#if defined(HAVE_SYCL) || defined(HAVE_CUDA) || defined(HAVE_HIP) || defined(HAVE_METAL)
        if (!backend_active(requested_backend, &state->gpu))
            return reject_inactive_feature_backend(state, feature.name, requested_backend);
#else
        return reject_inactive_feature_backend(state, feature.name, requested_backend);
#endif
    }
    if (vmaf_use_feature(state->vmaf, feature.name, feature.opts_dict)) {
        (void)fprintf(stderr, "problem loading feature extractor: %s\n", feature.name);
        return -1;
    }
    return 0;
}

[[nodiscard]] int register_cli_features(CliRunState *state)
{
    for (unsigned i = 0; i < state->c.feature_cnt; i++) {
        const int err = register_cli_feature(state, i);
        if (err)
            return err;
    }
    return 0;
}

} // namespace

namespace
{

[[nodiscard]] const char *active_backend_name(const GpuStates *states)
{
    const char *backend = "cpu";
#ifdef HAVE_SYCL
    if (states->sycl_active)
        backend = "sycl";
#endif
#ifdef HAVE_HIP
    if (states->hip_active)
        backend = "hip";
#endif
#ifdef HAVE_METAL
    if (states->metal_active)
        backend = "metal";
#endif
#ifdef HAVE_CUDA
    if (states->cuda_active)
        backend = "cuda";
#endif
    (void)states;
    return backend;
}

[[nodiscard]] int write_cli_output(const CliRunState *state)
{
    if (!state->c.output_path)
        return 0;
    const int err = vmaf_write_output_with_format(state->vmaf, state->c.output_path,
                                                  state->c.output_fmt, state->c.precision_fmt);
    if (err) {
        (void)fprintf(stderr, "problem writing output to %s (err=%d)\n", state->c.output_path, err);
        return err;
    }
    amend_json_with_backend_used(state->c.output_path, state->c.output_fmt,
                                 active_backend_name(&state->gpu));
    return 0;
}

} // namespace

namespace
{

[[nodiscard]] int score_cli_inputs(CliRunState *state, int istty)
{
    skip_initial_frames(state->vmaf, &state->vid_ref, &state->vid_dist, &state->c,
                        state->common_bitdepth);
    const FrameLoopResult loop = run_frame_loop(state->vmaf, &state->vid_ref, &state->vid_dist,
                                                &state->c, state->common_bitdepth, istty);
    if (loop.exit_code)
        return loop.exit_code;
    if (loop.frames == 0) {
        (void)fprintf(stderr, "no frames decoded from \"%s\" / \"%s\"\n", state->c.path_ref,
                      state->c.path_dist);
        return VMAF_EXIT_NO_FRAMES_DECODED;
    }
    const int flush_err = vmaf_read_pictures(state->vmaf, nullptr, nullptr, 0);
    if (flush_err) {
        (void)fprintf(stderr, "problem flushing context\n");
        return flush_err;
    }
    if (!state->c.no_prediction) {
        const int score_err =
            report_pooled_scores(state->vmaf, &state->c, state->arrays, loop.frames, istty);
        if (score_err)
            return score_err;
    }
    return write_cli_output(state);
}

[[nodiscard]] int run_cli(CliRunState *state, int istty)
{
    print_cli_banner(&state->c, istty);
    int err = open_cli_inputs(state);
    if (err)
        return -1;
    err = init_cli_context(state);
    if (err)
        return err;
    err = init_cli_backends(state);
    if (err)
        return err;
    err = preallocate_cli_pictures(state, istty);
    if (err)
        return err;
    err = load_cli_models(state);
    if (err)
        return err;
    err = register_cli_features(state);
    if (err)
        return err;
    if (configure_tiny_model(state->vmaf, &state->c))
        return -1;
    return score_cli_inputs(state, istty);
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef _WIN32
    /* cli_parse exits directly for --help, --version, and parse errors. Static
     * storage ensures the console restoration destructor runs on those paths. */
    static const WindowsConsoleGuard console_guard;
#endif
    CliRunState state = {};
    cli_parse(argc, argv, &state.c);
    const CliRunGuard guard(&state);
    return run_cli(&state, isatty(fileno(stderr)));
}
