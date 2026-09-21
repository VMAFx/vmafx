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
 * Conservative idioms: nullptr, static_cast, [[nodiscard]], and RAII
 * wrappers for the three pointer-owning arrays (model, model_collection,
 * model_collection_label). CliRunState plus cleanup_cli_run() preserve the
 * required subsystem teardown order without cleanup jumps. Spinner header
 * uses inline to suppress ODR warnings. */

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
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
    FILE *fp = fopen(output_path, "wb");
    const int file_error = fp ? 0 : (errno ? errno : EIO);
#else
    const int raw_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    FILE *fp = (raw_fd >= 0) ? fdopen(raw_fd, "wb") : nullptr;
    const int file_error = fp ? 0 : (errno ? errno : EIO);
    if (!fp && raw_fd >= 0) {
        /* POSIX leaves the descriptor open when fdopen() fails, so closing it here is
         * required.  cppcheck's posix.cfg lists fdopen as a deallocator of the fd
         * unconditionally, so 2.13 — the version CI installs from apt — reads this as a
         * second free.  2.21 no longer does. */
        /* cppcheck-suppress doubleFree ; see the note above */
        if (close(raw_fd) != 0) {
            (void)fprintf(stderr,
                          "vmaf: could not close failed output descriptor for %s (errno=%d)\n",
                          output_path, errno ? errno : EIO);
        }
    }
#endif
    if (!fp) {
        (void)fprintf(stderr, "vmaf: could not write backend error JSON to %s (errno=%d)\n",
                      output_path, file_error);
        return;
    }
    /* Keep the JSON compact + single line — every consumer in the tree
     * parses it with a permissive reader and the file is short. */
    const int write_result =
        fprintf(fp,
                "{\"error\": \"%s\", \"backend_requested\": \"%s\", "
                "\"errno\": %d, \"adr\": \"ADR-0498\", "
                "\"exit_code\": %d}\n",
                reason, backend_requested, err_no, VMAF_EXIT_BACKEND_INIT_FAILED);
    const int write_errno = write_result < 0 ? errno : 0;
    const int close_result = fclose(fp);
    const int close_errno = close_result != 0 ? errno : 0;
    if (write_result < 0 || close_result != 0) {
        const int output_errno = write_result < 0 ? write_errno : close_errno;
        (void)fprintf(stderr, "vmaf: could not finish backend error JSON %s (errno=%d)\n",
                      output_path, output_errno ? output_errno : EIO);
    }
}

/* Validate per-video constraints that do not require comparing the two streams:
 * supported bitdepth range and positive (non-zero) frame dimensions. */
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

template <typename Sample>
void copy_same_depth_plane(VmafPicture *pic, const video_input_ycbcr ycbcr,
                           const video_input_info *info, unsigned plane)
{
    const int xdec = plane && !(info->pixel_fmt & 1);
    const int ydec = plane && !(info->pixel_fmt & 2);
    const ptrdiff_t source_stride = ycbcr[plane].stride / static_cast<ptrdiff_t>(sizeof(Sample));
    const Sample *source = reinterpret_cast<const Sample *>(ycbcr[plane].data) +
                           static_cast<size_t>(info->pic_y >> ydec) * source_stride +
                           (info->pic_x >> xdec);
    Sample *destination = static_cast<Sample *>(pic->data[plane]);
    const ptrdiff_t destination_stride =
        pic->stride[plane] / static_cast<ptrdiff_t>(sizeof(Sample));
    for (unsigned row = 0; row < pic->h[plane]; row++) {
        memcpy(destination, source, sizeof(*destination) * pic->w[plane]);
        destination += destination_stride;
        source += source_stride;
    }
}

template <typename Sample>
void copy_shifted_plane(VmafPicture *pic, const video_input_ycbcr ycbcr,
                        const video_input_info *info, unsigned plane, int left_shift)
{
    const int xdec = plane && !(info->pixel_fmt & 1);
    const int ydec = plane && !(info->pixel_fmt & 2);
    const ptrdiff_t source_stride = ycbcr[plane].stride / static_cast<ptrdiff_t>(sizeof(Sample));
    const Sample *source = reinterpret_cast<const Sample *>(ycbcr[plane].data) +
                           static_cast<size_t>(info->pic_y >> ydec) * source_stride +
                           (info->pic_x >> xdec);
    auto *destination = static_cast<uint16_t *>(pic->data[plane]);
    const ptrdiff_t destination_stride = pic->stride[plane] / 2;
    for (unsigned row = 0; row < pic->h[plane]; row++) {
        for (unsigned column = 0; column < pic->w[plane]; column++) {
            destination[column] = static_cast<uint16_t>(source[column] << left_shift);
        }
        destination += destination_stride;
        source += source_stride;
    }
}

/* Copy video input data to the picture buffer. Compile-time sample types keep
 * the per-pixel path direct while the dispatcher stays independent of plane
 * traversal and row-stride mechanics. */
void copy_picture_data(VmafPicture *pic, video_input_ycbcr ycbcr, const video_input_info *info,
                       int depth)
{
    if (info->depth == depth) {
        for (unsigned plane = 0; plane < 3; plane++) {
            if (info->depth == 8) {
                copy_same_depth_plane<uint8_t>(pic, ycbcr, info, plane);
            } else {
                copy_same_depth_plane<uint16_t>(pic, ycbcr, info, plane);
            }
        }
        return;
    }
    if (depth <= 8) {
        return;
    }
    const int left_shift = depth - info->depth;
    for (unsigned plane = 0; plane < 3; plane++) {
        if (info->depth == 8) {
            copy_shifted_plane<uint8_t>(pic, ycbcr, info, plane, left_shift);
        } else {
            copy_shifted_plane<uint16_t>(pic, ycbcr, info, plane, left_shift);
        }
    }
}

[[nodiscard]] int finish_unread_picture(VmafPicture *pic, int fetch_ret)
{
    const int err_unref = vmaf_picture_unref(pic);
    if (err_unref)
        (void)fprintf(stderr, "\nproblem during vmaf_picture_unref (unread)\n");
    return fetch_ret == 0 ? 1 : -1;
}

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

/* RAII wrapper for the three parallel model-tracking arrays.
 * Owns heap-allocated VmafModel**, VmafModelCollection**, and const char**
 * arrays sized to model_cnt. The destructor calls vmaf_model_destroy /
 * vmaf_model_collection_destroy and frees the backing store when the owning
 * CliRunState leaves scope.
 *
 * ADR-0809: replaces the manual free()/vmaf_model*_destroy() calls that
 * were previously duplicated across three locations in main(). */
class ModelArrays
{
  private:
    VmafModel **m_model{nullptr};
    VmafModelCollection **m_collection{nullptr};
    const char **m_collection_label{nullptr};
    unsigned m_model_cnt{0};
    unsigned m_collection_cnt{0};

  public:
    ModelArrays() = default;

    /* Non-copyable, non-moveable */
    ModelArrays(const ModelArrays &) = delete;
    ModelArrays &operator=(const ModelArrays &) = delete;
    ModelArrays(ModelArrays &&) = delete;
    ModelArrays &operator=(ModelArrays &&) = delete;

    [[nodiscard]] VmafModel **model() noexcept
    {
        return m_model;
    }
    [[nodiscard]] VmafModel *const *model() const noexcept
    {
        return m_model;
    }
    [[nodiscard]] VmafModelCollection **collection() noexcept
    {
        return m_collection;
    }
    [[nodiscard]] VmafModelCollection *const *collection() const noexcept
    {
        return m_collection;
    }
    [[nodiscard]] const char **collection_label() noexcept
    {
        return m_collection_label;
    }
    [[nodiscard]] const char *const *collection_label() const noexcept
    {
        return m_collection_label;
    }
    [[nodiscard]] unsigned &collection_cnt() noexcept
    {
        return m_collection_cnt;
    }
    [[nodiscard]] unsigned collection_cnt() const noexcept
    {
        return m_collection_cnt;
    }

    [[nodiscard]] int allocate(unsigned cnt)
    {
        m_model_cnt = cnt;
        if (cnt == 0)
            return 0;

        m_model = static_cast<VmafModel **>(malloc(sizeof(*m_model) * cnt));
        if (!m_model)
            return -1;
        (void)memset(static_cast<void *>(m_model), 0, sizeof(*m_model) * cnt);

        m_collection = static_cast<VmafModelCollection **>(malloc(sizeof(*m_collection) * cnt));
        if (!m_collection)
            return -1;
        (void)memset(static_cast<void *>(m_collection), 0, sizeof(*m_collection) * cnt);

        m_collection_label = static_cast<const char **>(malloc(sizeof(*m_collection_label) * cnt));
        if (!m_collection_label)
            return -1;
        (void)memset(static_cast<void *>(m_collection_label), 0, sizeof(*m_collection_label) * cnt);

        return 0;
    }

    ~ModelArrays()
    {
        if (m_model) {
            for (unsigned i = 0; i < m_model_cnt; i++)
                vmaf_model_destroy(m_model[i]);
            free(static_cast<void *>(m_model));
        }
        if (m_collection) {
            for (unsigned i = 0; i < m_collection_cnt; i++)
                vmaf_model_collection_destroy(m_collection[i]);
            free(static_cast<void *>(m_collection));
        }
        free(static_cast<void *>(m_collection_label));
    }
};

/* Helper: pick the human-readable label (version preferred over path) for
 * the given model-config entry, used in error messages.
 */
const char *model_label(const CLISettings *c, unsigned i)
{
    return c->model_config[i].version ? c->model_config[i].version : c->model_config[i].path;
}

/* Initialise a model-collection slot for entry `i`. The caller passes the
 * current `*slot` index; on any failure path this helper bumps `*slot`
 * before returning so the caller's cleanup loop unwinds the partially
 * initialised entry. Returns 0 on success.
 */
[[nodiscard]] int load_model_collection_entry(VmafContext *vmaf, CLISettings *c, unsigned i,
                                              ModelArrays &arrays, unsigned w, unsigned h,
                                              enum VmafPixelFormat pix_fmt)
{
    unsigned *slot = &arrays.collection_cnt();
    int err = 0;

    if (c->model_config[i].version) {
        err = vmaf_model_collection_load(&arrays.model()[i], &arrays.collection()[*slot],
                                         &c->model_config[i].cfg, c->model_config[i].version);
    } else {
        err =
            vmaf_model_collection_load_from_path(&arrays.model()[i], &arrays.collection()[*slot],
                                                 &c->model_config[i].cfg, c->model_config[i].path);
    }

    if (err) {
        (void)fprintf(stderr, "problem loading model: %s\n", model_label(c, i));
        return -1;
    }

    arrays.collection_label()[*slot] = model_label(c, i);

    char err_msg[512] = {0};
    if (vmaf_validate_model_dimensions(arrays.model()[i], model_label(c, i), w, h, pix_fmt, err_msg,
                                       sizeof(err_msg))) {
        (void)fprintf(stderr, "error: %s.%s\n", err_msg,
                      c->model_config[i].is_default ?
                          " Pass --model explicitly to use a different model." :
                          "");
        (*slot)++;
        return -EINVAL;
    }

    for (unsigned j = 0; j < c->model_config[i].overload_cnt; j++) {
        err = vmaf_model_collection_feature_overload(
            arrays.model()[i], &arrays.collection()[*slot],
            c->model_config[i].feature_overload[j].name,
            c->model_config[i].feature_overload[j].opts_dict);
        if (err) {
            (void)fprintf(stderr,
                          "problem overloading feature extractors from model collection: %s\n",
                          model_label(c, i));
            (*slot)++;
            return -1;
        }
    }

    err = vmaf_use_features_from_model_collection(vmaf, arrays.collection()[*slot]);
    if (err) {
        (void)fprintf(stderr, "problem loading feature extractors from model collection: %s\n",
                      model_label(c, i));
        (*slot)++;
        return -1;
    }

    (*slot)++;
    return 0;
}

/* Load a single model entry from the CLI configuration. Handles the model
 * vs model-collection fallback that the `--model` option's overloaded
 * semantics require.
 */
[[nodiscard]] int load_one_model_entry(VmafContext *vmaf, CLISettings *c, unsigned i,
                                       ModelArrays &arrays, unsigned w, unsigned h,
                                       enum VmafPixelFormat pix_fmt)
{
    int err;

    if (c->model_config[i].version) {
        err = vmaf_model_load(&arrays.model()[i], &c->model_config[i].cfg,
                              c->model_config[i].version);
    } else {
        err = vmaf_model_load_from_path(&arrays.model()[i], &c->model_config[i].cfg,
                                        c->model_config[i].path);
    }

    /* `--model` is overloaded: if a single-model load fails, fall back to
     * loading the same identifier as a model collection.
     */
    if (err) {
        return load_model_collection_entry(vmaf, c, i, arrays, w, h, pix_fmt);
    }

    char err_msg[512] = {0};
    if (vmaf_validate_model_dimensions(arrays.model()[i], model_label(c, i), w, h, pix_fmt, err_msg,
                                       sizeof(err_msg))) {
        (void)fprintf(stderr, "error: %s.%s\n", err_msg,
                      c->model_config[i].is_default ?
                          " Pass --model explicitly to use a different model." :
                          "");
        return -EINVAL;
    }

    for (unsigned j = 0; j < c->model_config[i].overload_cnt; j++) {
        err = vmaf_model_feature_overload(arrays.model()[i],
                                          c->model_config[i].feature_overload[j].name,
                                          c->model_config[i].feature_overload[j].opts_dict);
        if (err) {
            (void)fprintf(stderr, "problem overloading feature extractors from model: %s\n",
                          model_label(c, i));
            return -1;
        }
    }

    err = vmaf_use_features_from_model(vmaf, arrays.model()[i]);
    if (err) {
        (void)fprintf(stderr, "problem loading feature extractors from model: %s\n",
                      model_label(c, i));
        return -1;
    }

    return 0;
}

/* Open both reference and distorted input streams (raw YUV via raw_input_open
 * when --use_yuv is set, otherwise the codec auto-detection path via
 * video_input_open). On success transfers FILE* ownership from *file_ref/dist
 * to the corresponding video_input and zeros the pointers so the cleanup
 * fclose() doesn't double-close. Sets *vid_ref_open / *vid_dist_open to true
 * for cleanup unwinding. Returns 0 on success and -1 on any failure.
 */
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

[[nodiscard]] bool is_explicit_backend(const CLISettings *c)
{
    return c->backend && strcmp(c->backend, "auto") != 0 && strcmp(c->backend, "cpu") != 0;
}

[[nodiscard]] bool backend_is_compiled(const char *backend)
{
    (void)backend;
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

[[nodiscard]] int validate_explicit_backend(const CLISettings *c, bool explicit_backend)
{
    if (!explicit_backend || backend_is_compiled(c->backend))
        return 0;
    (void)fprintf(stderr,
                  "vmaf: --backend %s requested but this libvmaf was built without %s support; "
                  "refusing to silently fall back to CPU (ADR-0498)\n",
                  c->backend, c->backend);
    write_backend_error_json(c->output_path, c->output_fmt, c->backend,
                             "backend not compiled into this libvmaf", 0);
    return VMAF_INIT_GPU_EXPLICIT_FAIL;
}

#ifdef HAVE_SYCL
[[nodiscard]] int init_sycl_backend(VmafContext *vmaf, const CLISettings *c, VmafSyclState **state,
                                    bool *active, bool explicit_backend)
{
    if ((c->sycl_device < 0 && !c->use_gpumask) || c->no_sycl)
        return 0;
    const VmafSyclConfiguration cfg = {
        .device_index = c->sycl_device >= 0 ? c->sycl_device : 0,
        .enable_profiling = 0,
    };
    const int init_err = vmaf_sycl_state_init(state, cfg);
    if (init_err) {
        (void)fprintf(stderr, "problem during vmaf_sycl_state_init, using CPU\n");
        if (!explicit_backend || strcmp(c->backend, "sycl") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend sycl requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "sycl",
                                 "vmaf_sycl_state_init failed", init_err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    if (vmaf_sycl_import_state(vmaf, *state)) {
        (void)fprintf(stderr, "problem during vmaf_sycl_import_state\n");
        return -1;
    }
    *active = true;
    return 0;
}
#endif

#ifdef HAVE_CUDA
[[nodiscard]] int init_cuda_backend(VmafContext *vmaf, const CLISettings *c, VmafCudaState **state,
                                    bool sycl_active, bool *active, bool explicit_backend)
{
    *active = false;
    if (!c->use_gpumask || c->no_cuda || sycl_active)
        return 0;
    const VmafCudaConfiguration cfg = {};
    const int init_err = vmaf_cuda_state_init(state, cfg);
    if (init_err) {
        (void)fprintf(stderr, "problem during vmaf_cuda_state_init, using CPU\n");
        if (!explicit_backend || strcmp(c->backend, "cuda") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend cuda requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "cuda",
                                 "vmaf_cuda_state_init failed", init_err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    if (vmaf_cuda_import_state(vmaf, *state)) {
        (void)fprintf(stderr, "problem during vmaf_cuda_import_state\n");
        return -1;
    }
    *active = true;
    return 0;
}
#endif

#ifdef HAVE_HIP
[[nodiscard]] int init_hip_backend(VmafContext *vmaf, const CLISettings *c, VmafHipState **state,
                                   bool *active, bool explicit_backend)
{
    if (c->hip_device < 0 || c->no_hip)
        return 0;
    const VmafHipConfiguration cfg = {.device_index = c->hip_device, .flags = 0};
    const int init_err = vmaf_hip_state_init(state, cfg);
    if (init_err) {
        (void)fprintf(stderr, "problem during vmaf_hip_state_init (%d), using CPU\n", init_err);
        if (!explicit_backend || strcmp(c->backend, "hip") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend hip requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "hip", "vmaf_hip_state_init failed",
                                 init_err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    if (vmaf_hip_import_state(vmaf, *state)) {
        (void)fprintf(stderr, "problem during vmaf_hip_import_state\n");
        return -1;
    }
    *active = true;
    return 0;
}
#endif

#ifdef HAVE_METAL
[[nodiscard]] int init_metal_backend(VmafContext *vmaf, const CLISettings *c,
                                     VmafMetalState **state, bool *active, bool explicit_backend)
{
    if (c->metal_device < 0 || c->no_metal)
        return 0;
    const VmafMetalConfiguration cfg = {.device_index = c->metal_device, .flags = 0};
    const int init_err = vmaf_metal_state_init(state, cfg);
    if (init_err) {
        (void)fprintf(stderr, "problem during vmaf_metal_state_init (%d), using CPU\n", init_err);
        if (!explicit_backend || strcmp(c->backend, "metal") != 0)
            return 0;
        (void)fprintf(stderr, "vmaf: --backend metal requested but init failed; refusing to "
                              "silently fall back to CPU (ADR-0498)\n");
        write_backend_error_json(c->output_path, c->output_fmt, "metal",
                                 "vmaf_metal_state_init failed", init_err);
        return VMAF_INIT_GPU_EXPLICIT_FAIL;
    }
    if (vmaf_metal_import_state(vmaf, *state)) {
        (void)fprintf(stderr, "problem during vmaf_metal_import_state\n");
        return -1;
    }
    *active = true;
    return 0;
}
#endif

/* Initialise GPU backends in the priority chain SYCL > CUDA > HIP > Metal. */
[[nodiscard]] int init_gpu_backends(VmafContext *vmaf, const CLISettings *c
#ifdef HAVE_SYCL
                                    ,
                                    VmafSyclState **sycl_state, bool *sycl_active
#endif
#ifdef HAVE_CUDA
                                    ,
                                    VmafCudaState **cuda_state, bool *cuda_active
#endif
#ifdef HAVE_HIP
                                    ,
                                    VmafHipState **hip_state, bool *hip_active
#endif
#ifdef HAVE_METAL
                                    ,
                                    VmafMetalState **metal_state, bool *metal_active
#endif
)
{
    (void)vmaf;
    const bool explicit_backend = is_explicit_backend(c);
    const int validation_err = validate_explicit_backend(c, explicit_backend);
    if (validation_err)
        return validation_err;
#ifdef HAVE_SYCL
    const int sycl_err = init_sycl_backend(vmaf, c, sycl_state, sycl_active, explicit_backend);
    if (sycl_err)
        return sycl_err;
#endif
#ifdef HAVE_CUDA
#ifdef HAVE_SYCL
    const bool sycl_was_activated = *sycl_active;
#else
    const bool sycl_was_activated = false;
#endif
    const int cuda_err =
        init_cuda_backend(vmaf, c, cuda_state, sycl_was_activated, cuda_active, explicit_backend);
    if (cuda_err)
        return cuda_err;
#endif
#ifdef HAVE_HIP
    const int hip_err = init_hip_backend(vmaf, c, hip_state, hip_active, explicit_backend);
    if (hip_err)
        return hip_err;
#endif
#ifdef HAVE_METAL
    const int metal_err = init_metal_backend(vmaf, c, metal_state, metal_active, explicit_backend);
    if (metal_err)
        return metal_err;
#endif
    return 0;
}

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

/* Returns 1 when the named backend is active in this run (state_init
 * succeeded and the matching ``--<backend>_device`` was requested),
 * 0 otherwise. The active flags live in main() so this helper accepts
 * each as a parameter. */
[[nodiscard]] int backend_active(const char *backend, bool sycl_act, bool cuda_act, bool hip_act,
                                 bool metal_act)
{
    if (!strcmp(backend, "sycl"))
        return sycl_act ? 1 : 0;
    if (!strcmp(backend, "cuda"))
        return cuda_act ? 1 : 0;
    if (!strcmp(backend, "hip"))
        return hip_act ? 1 : 0;
    if (!strcmp(backend, "metal"))
        return metal_act ? 1 : 0;
    return 0;
}

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

/* Configure the tiny-AI (DNN) model on the VMAF context when --tiny-model
 * is passed. Performs the optional Sigstore-bundle verification (T6-9 /
 * ADR-0211) before opening the model so a signature failure short-circuits
 * load and never touches ORT. Returns 0 on success and -1 on any failure.
 */
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

/* Skip the first `c->frame_skip_ref` ref frames and `c->frame_skip_dist` dist
 * frames, releasing each one back to the picture pool. fetch_picture() reserves
 * a slot from the preallocated pool, and skipped frames are never handed to
 * vmaf_read_pictures() to release them; without unref the pool is exhausted
 * after N skips and the next fetch blocks indefinitely.
 */
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
 * restore whatever was there on every exit. Whatever the console refuses is
 * reflected back through console_progress_style(), which then falls back to
 * the ASCII table and space padding. No effect on POSIX: the whole class is
 * #ifdef'd out.
 */
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

#endif /* _WIN32 */

namespace
{

/* Glyph table + erase-to-EOL sequence for the interactive progress line. */
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

/*
 * Emit one interactive progress line. Netflix/vmaf#743: the glyph table and
 * the erase-to-EOL sequence come from the console's actual capabilities, so a
 * Windows console that refuses UTF-8 or VT gets ASCII plus space padding
 * instead of mojibake and a literal erase sequence. On POSIX the style is
 * always the braille table plus the CSI erase, i.e. the emitted bytes are
 * identical to the previous unconditional form.
 */
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

/* Outcome of the per-frame fetch + process loop.
 *
 * `frames` is the number of frames successfully consumed (the post-increment
 * `picture_index` value the pooling path uses to compute `picture_index - 1`).
 * `exit_code` is 0 when the loop stopped for a benign reason -- end of either
 * stream, or c->frame_cnt reached -- and non-zero when it stopped because a
 * read failed. Reporting only the count, as this loop used to, gave main() no
 * way to tell the two apart. */
struct FrameLoopResult {
    unsigned frames;
    int exit_code;
};

/* Drive the main per-frame fetch + process loop. Stops at EOF on either side,
 * on read errors, or when c->frame_cnt is reached; see FrameLoopResult for how
 * the caller tells those apart.
 */
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

        if (ret1 || ret2) {
            if (!ret1) {
                const int err_unref = vmaf_picture_unref(&pic_ref);
                if (err_unref)
                    (void)fprintf(stderr, "\nproblem during vmaf_picture_unref\n");
            }
            if (!ret2) {
                const int err_unref = vmaf_picture_unref(&pic_dist);
                if (err_unref)
                    (void)fprintf(stderr, "\nproblem during vmaf_picture_unref\n");
            }
        }

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

/* Compute and report pooled VMAF scores for all loaded models and model
 * collections. Called only when c->no_prediction is false. Returns 0 on
 * success and non-zero on the first per-model scoring failure.
 */
[[nodiscard]] int report_pooled_scores(VmafContext *vmaf, const CLISettings *c,
                                       const ModelArrays &arrays, unsigned picture_index, int istty)
{
    for (unsigned i = 0; i < c->model_cnt; i++) {
        double vmaf_score;
        const int err = vmaf_score_pooled(vmaf, arrays.model()[i], VMAF_POOL_METHOD_MEAN,
                                          &vmaf_score, 0, picture_index - 1);
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

    for (unsigned i = 0; i < arrays.collection_cnt(); i++) {
        VmafModelCollectionScore score = {.type = static_cast<VmafModelCollectionScoreType>(0),
                                          .bootstrap = {}};
        const int err = vmaf_score_pooled_model_collection(
            vmaf, arrays.collection()[i], VMAF_POOL_METHOD_MEAN, &score, 0, picture_index - 1);
        if (err) {
            (void)fprintf(stderr, "problem generating pooled VMAF score\n");
            return -1;
        }

        /* VmafModelCollectionScoreType has only BOOTSTRAP as a printable
         * variant (UNKNOWN is a no-op), so a plain if is clearer than a
         * two-label switch. */
        if (score.type == VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP && istty &&
            (!c->quiet || !c->output_path)) {
            (void)fprintf(stderr, "%s: ", arrays.collection_label()[i]);
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

/* ADR-0498 / Bug #v2-E: amend the JSON output file with a top-level
 * ``"backend_used": "NAME"`` key so downstream consumers (CI gates,
 * MCP probes per PR #1251) can confirm which backend actually ran.
 * Implemented as a textual edit on the closing ``}`` to avoid pulling
 * a JSON parser into the CLI; the writer always emits a single
 * top-level object so the brace is at the file tail.
 *
 * No-op when output_path is NULL or format isn't JSON.
 *
 * The annotation is optional and only applies to a seekable regular file. By
 * the time it runs the scores are computed and the JSON body is already on
 * disk, so an output target that cannot be re-opened and rewritten in place —
 * `--output /dev/null`, `--output /dev/stdout` on a pipe, a process
 * substitution — is not a run failure. Those targets are documented CLI
 * idioms (see the pages under docs/metrics and the reproducers in
 * docs/state.md) and
 * exited 0 before the CliRunState refactor; escalating them would break every
 * caller that discards or pipes the JSON. Only a failure *after* the in-place
 * rewrite began can leave the file inconsistent, and that reaches the exit
 * status. Every outcome is reported on stderr either way, so nothing is
 * silently swallowed.
 */
struct JsonAmendResult {
    int error;            /* negative errno, or 0 */
    bool rewrite_started; /* the in-place edit had begun when `error` was set */
};

[[nodiscard]] JsonAmendResult finish_amend(FILE *fp, const char *output_path, int prior_error,
                                           bool rewrite_started)
{
    int error = prior_error;
    if (fclose(fp) != 0) {
        const int close_error = errno ? -errno : -EIO;
        (void)fprintf(stderr, "problem closing output file %s (err=%d)\n", output_path,
                      close_error);
        if (!error)
            error = close_error;
    }
    return {error, rewrite_started};
}

[[nodiscard]] JsonAmendResult amend_json_with_backend_used(const char *output_path,
                                                           enum VmafOutputFormat fmt,
                                                           const char *backend_used)
{
    if (!output_path || !backend_used)
        return {0, false};
    if (fmt != VMAF_OUTPUT_FORMAT_JSON)
        return {0, false};

    FILE *fp = fopen(output_path, "rb+");
    if (!fp)
        return {errno ? -errno : -EIO, false};
    if (fseek(fp, 0, SEEK_END) != 0) {
        const int seek_error = errno ? -errno : -EIO;
        return finish_amend(fp, output_path, seek_error, false);
    }
    const long size = ftell(fp);
    if (size <= 1) {
        const int size_error = size < 0 && errno ? -errno : -EINVAL;
        return finish_amend(fp, output_path, size_error, false);
    }
    /* Walk backwards over trailing whitespace + the final '}'. */
    long pos = size - 1;
    bool found_closing_brace = false;
    while (pos > 0) {
        if (fseek(fp, pos, SEEK_SET) != 0) {
            const int seek_error = errno ? -errno : -EIO;
            return finish_amend(fp, output_path, seek_error, false);
        }
        const int ch = fgetc(fp);
        if (ch == EOF) {
            const int read_error = ferror(fp) && errno ? -errno : -EINVAL;
            return finish_amend(fp, output_path, read_error, false);
        }
        if (ch == '}') {
            found_closing_brace = true;
            break;
        }
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            return finish_amend(fp, output_path, -EINVAL, false);
        }
        pos--;
    }
    if (!found_closing_brace)
        return finish_amend(fp, output_path, -EINVAL, false);
    if (fseek(fp, pos, SEEK_SET) != 0) {
        const int seek_error = errno ? -errno : -EIO;
        return finish_amend(fp, output_path, seek_error, false);
    }
    const int write_result = fprintf(fp, ", \"backend_used\": \"%s\"}\n", backend_used);
    const int write_error = write_result < 0 ? (errno ? -errno : -EIO) : 0;
    return finish_amend(fp, output_path, write_error, true);
}

/* Resources acquired after cli_parse(). cleanup_cli_run() preserves the
 * historical unwind order; ModelArrays is destroyed when this aggregate leaves
 * main(), after the explicit cleanup call. */
struct CliRunState {
    CLISettings settings{};
    FILE *file_ref = nullptr;
    FILE *file_dist = nullptr;
    bool vid_ref_open = false;
    bool vid_dist_open = false;
    video_input vid_ref = {.vtbl = nullptr, .ctx = nullptr, .fin = nullptr};
    video_input vid_dist = {.vtbl = nullptr, .ctx = nullptr, .fin = nullptr};
    VmafContext *vmaf = nullptr;
    ModelArrays arrays;
#ifdef HAVE_SYCL
    bool sycl_active = false;
    VmafSyclState *sycl_state = nullptr;
#endif
#ifdef HAVE_CUDA
    bool cuda_active = false;
    VmafCudaState *cuda_state = nullptr;
#endif
#ifdef HAVE_HIP
    bool hip_active = false;
    VmafHipState *hip_state = nullptr;
#endif
#ifdef HAVE_METAL
    bool metal_active = false;
    VmafMetalState *metal_state = nullptr;
#endif
};

[[nodiscard]] int cleanup_cli_run(CliRunState *run)
{
    int cleanup_error = 0;
    if (run->vmaf) {
        cleanup_error = vmaf_close(run->vmaf);
        run->vmaf = nullptr;
        if (cleanup_error)
            (void)fprintf(stderr, "problem closing VMAF context (err=%d)\n", cleanup_error);
    }
#ifdef HAVE_SYCL
    if (run->sycl_state)
        vmaf_sycl_state_free(&run->sycl_state);
#endif
#ifdef HAVE_CUDA
    const int cuda_free_err = run->cuda_state ? vmaf_cuda_state_free(run->cuda_state) : 0;
    run->cuda_state = nullptr;
    if (cuda_free_err) {
        (void)fprintf(stderr, "problem freeing CUDA state (err=%d)\n", cuda_free_err);
        if (!cleanup_error)
            cleanup_error = cuda_free_err;
    }
#endif
#ifdef HAVE_HIP
    if (run->hip_state)
        vmaf_hip_state_free(&run->hip_state);
#endif
#ifdef HAVE_METAL
    if (run->metal_state)
        vmaf_metal_state_free(&run->metal_state);
#endif
    if (run->vid_dist_open)
        video_input_close(&run->vid_dist);
    if (run->vid_ref_open)
        video_input_close(&run->vid_ref);
    if (run->file_dist) {
        const int close_error = fclose(run->file_dist) == 0 ? 0 : (errno ? -errno : -EIO);
        run->file_dist = nullptr;
        if (close_error) {
            (void)fprintf(stderr, "problem closing distorted input (err=%d)\n", close_error);
            if (!cleanup_error)
                cleanup_error = close_error;
        }
    }
    if (run->file_ref) {
        const int close_error = fclose(run->file_ref) == 0 ? 0 : (errno ? -errno : -EIO);
        run->file_ref = nullptr;
        if (close_error) {
            (void)fprintf(stderr, "problem closing reference input (err=%d)\n", close_error);
            if (!cleanup_error)
                cleanup_error = close_error;
        }
    }
    cli_free(&run->settings);
    return cleanup_error;
}

void print_version_banner(const CLISettings *c, int istty)
{
    if (!istty || c->quiet)
        return;
    if (!c->vmafx_mode || c->netflix_compat) {
        (void)fprintf(stderr, "VMAF version %s\n", vmaf_version());
        return;
    }
    if (c->precision_max) {
        (void)fprintf(stderr, "VMAFX version %s (precision=max)\n", vmaf_version());
    } else {
        (void)fprintf(stderr, "VMAFX version %s\n", vmaf_version());
    }
}

[[nodiscard]] int open_cli_inputs(CliRunState &run)
{
    const CLISettings *const c = &run.settings;
    /* No-reference mode needs two independent decoder handles because the
     * picture pair is released independently by vmaf_read_pictures(). */
    const char *const ref_path = c->no_reference ? c->path_dist : c->path_ref;
    run.file_ref = fopen(ref_path, "rb");
    if (!run.file_ref) {
        (void)fprintf(stderr, "could not open file: %s\n", ref_path);
        return -1;
    }
    run.file_dist = fopen(c->path_dist, "rb");
    if (!run.file_dist) {
        (void)fprintf(stderr, "could not open file: %s\n", c->path_dist);
        return -1;
    }
    return open_input_videos(c, &run.file_ref, &run.file_dist, &run.vid_ref, &run.vid_dist,
                             &run.vid_ref_open, &run.vid_dist_open);
}

[[nodiscard]] int common_input_bitdepth(CliRunState &run)
{
    if (run.settings.use_yuv)
        return static_cast<int>(run.settings.bitdepth);
    video_input_info ref_info;
    video_input_info dist_info;
    video_input_get_info(&run.vid_ref, &ref_info);
    video_input_get_info(&run.vid_dist, &dist_info);
    return ref_info.depth > dist_info.depth ? ref_info.depth : dist_info.depth;
}

[[nodiscard]] int initialize_cli_context(CliRunState &run)
{
    const CLISettings *const c = &run.settings;
    const VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = c->thread_cnt,
        .n_subsample = c->subsample,
        .cpumask = c->cpumask,
        .gpumask = c->gpumask,
    };
    if (vmaf_init(&run.vmaf, cfg)) {
        (void)fprintf(stderr, "problem initializing VMAF context\n");
        return -1;
    }
    const int gpu_rc = init_gpu_backends(run.vmaf, c
#ifdef HAVE_SYCL
                                         ,
                                         &run.sycl_state, &run.sycl_active
#endif
#ifdef HAVE_CUDA
                                         ,
                                         &run.cuda_state, &run.cuda_active
#endif
#ifdef HAVE_HIP
                                         ,
                                         &run.hip_state, &run.hip_active
#endif
#ifdef HAVE_METAL
                                         ,
                                         &run.metal_state, &run.metal_active
#endif
    );
    if (gpu_rc == VMAF_INIT_GPU_EXPLICIT_FAIL)
        return VMAF_EXIT_BACKEND_INIT_FAILED;
    return gpu_rc ? -1 : 0;
}

[[nodiscard]] VmafPictureConfiguration make_picture_configuration(CliRunState &run,
                                                                  int common_bitdepth)
{
    video_input_info info;
    video_input_get_info(&run.vid_ref, &info);
    return {
        .pic_params =
            {
                .w = static_cast<unsigned>(info.pic_w),
                .h = static_cast<unsigned>(info.pic_h),
                .bpc = static_cast<unsigned>(common_bitdepth),
                .pix_fmt = pix_fmt_map(info.pixel_fmt),
            },
        /* ref + dist, previous ref, and a pair per in-flight worker */
        .pic_cnt = 2 * (run.settings.thread_cnt + 1) + 1,
    };
}

[[nodiscard]] int prepare_picture_pool(CliRunState &run, const VmafPictureConfiguration &pic_cfg,
                                       int istty)
{
    if (vmaf_preallocate_pictures(run.vmaf, pic_cfg)) {
        (void)fprintf(stderr, "problem during vmaf_preallocate_pictures\n");
        return -1;
    }
    if (istty && !run.settings.quiet)
        (void)fprintf(stderr, "picture pool: %u pictures pre-allocated\n", pic_cfg.pic_cnt);
    return 0;
}

[[nodiscard]] int load_cli_models(CliRunState &run, const VmafPictureConfiguration &pic_cfg)
{
    CLISettings *const c = &run.settings;
    if (run.arrays.allocate(c->model_cnt))
        return -1;
    for (unsigned i = 0; i < c->model_cnt; i++) {
        if (load_one_model_entry(run.vmaf, c, i, run.arrays, pic_cfg.pic_params.w,
                                 pic_cfg.pic_params.h, pic_cfg.pic_params.pix_fmt))
            return -EINVAL;
    }
    return 0;
}

[[nodiscard]] bool run_backend_active(const CliRunState &run, const char *backend)
{
    (void)run;
    const bool sycl =
#ifdef HAVE_SYCL
        run.sycl_active;
#else
        false;
#endif
    const bool cuda =
#ifdef HAVE_CUDA
        run.cuda_active;
#else
        false;
#endif
    const bool hip =
#ifdef HAVE_HIP
        run.hip_active;
#else
        false;
#endif
    const bool metal =
#ifdef HAVE_METAL
        run.metal_active;
#else
        false;
#endif
    return backend_active(backend, sycl, cuda, hip, metal) != 0;
}

[[nodiscard]] int register_cli_feature(CliRunState &run, const CLIFeatureConfig &feature,
                                       const VmafPictureConfiguration &pic_cfg)
{
    char feature_error[256] = {0};
    if (vmaf_validate_feature_dimensions(feature.name, pic_cfg.pic_params.w, pic_cfg.pic_params.h,
                                         pic_cfg.pic_params.pix_fmt, feature_error,
                                         sizeof(feature_error))) {
        (void)fprintf(stderr, "error: feature '%s' %s.\n", feature.name, feature_error);
        return -EINVAL;
    }
    const char *requested_backend = nullptr;
    if (feature_backend_suffix(feature.name, &requested_backend) &&
        !run_backend_active(run, requested_backend)) {
        (void)fprintf(stderr,
                      "vmaf: --feature %s pinned to %s backend but %s is not active in this run; "
                      "refusing to silently fall back to CPU (ADR-0498)\n",
                      feature.name, requested_backend, requested_backend);
        write_backend_error_json(run.settings.output_path, run.settings.output_fmt,
                                 requested_backend, "feature pinned to inactive backend", 0);
        return VMAF_EXIT_BACKEND_INIT_FAILED;
    }
    if (vmaf_use_feature(run.vmaf, feature.name, feature.opts_dict)) {
        (void)fprintf(stderr, "problem loading feature extractor: %s\n", feature.name);
        return -1;
    }
    return 0;
}

[[nodiscard]] int register_cli_features(CliRunState &run, const VmafPictureConfiguration &pic_cfg)
{
    for (unsigned i = 0; i < run.settings.feature_cnt; i++) {
        const int err = register_cli_feature(run, run.settings.feature_cfg[i], pic_cfg);
        if (err)
            return err;
    }
    return 0;
}

[[nodiscard]] const char *active_backend_name(const CliRunState &run)
{
    (void)run;
    const char *backend = "cpu";
#ifdef HAVE_SYCL
    if (run.sycl_active)
        backend = "sycl";
#endif
#ifdef HAVE_HIP
    if (run.hip_active)
        backend = "hip";
#endif
#ifdef HAVE_METAL
    if (run.metal_active)
        backend = "metal";
#endif
#ifdef HAVE_CUDA
    if (run.cuda_active)
        backend = "cuda";
#endif
    return backend;
}

[[nodiscard]] int write_cli_output(CliRunState &run)
{
    const CLISettings *const c = &run.settings;
    if (!c->output_path)
        return 0;
    const int err =
        vmaf_write_output_with_format(run.vmaf, c->output_path, c->output_fmt, c->precision_fmt);
    if (err) {
        (void)fprintf(stderr, "problem writing output to %s (err=%d)\n", c->output_path, err);
        return err;
    }
    const JsonAmendResult amend =
        amend_json_with_backend_used(c->output_path, c->output_fmt, active_backend_name(run));
    if (amend.error && amend.rewrite_started) {
        (void)fprintf(stderr, "problem recording active backend in %s (err=%d)\n", c->output_path,
                      amend.error);
        return amend.error;
    }
    if (amend.error) {
        /* The output target does not support the in-place annotation; the
         * scores and the JSON body it already wrote are complete and correct,
         * so the run still succeeded. See amend_json_with_backend_used(). */
        (void)fprintf(stderr,
                      "vmaf: could not record active backend in %s (err=%d); "
                      "scores and output are unaffected\n",
                      c->output_path, amend.error);
    }
    return 0;
}

[[nodiscard]] int run_cli_frames(CliRunState &run, int common_bitdepth, int istty)
{
    const CLISettings *const c = &run.settings;
    if (configure_tiny_model(run.vmaf, c))
        return -1;
    skip_initial_frames(run.vmaf, &run.vid_ref, &run.vid_dist, c, common_bitdepth);
    const FrameLoopResult loop =
        run_frame_loop(run.vmaf, &run.vid_ref, &run.vid_dist, c, common_bitdepth, istty);
    if (loop.exit_code)
        return loop.exit_code;
    if (loop.frames == 0) {
        (void)fprintf(stderr, "no frames decoded from \"%s\" / \"%s\"\n", c->path_ref,
                      c->path_dist);
        return VMAF_EXIT_NO_FRAMES_DECODED;
    }
    const int flush_err = vmaf_read_pictures(run.vmaf, nullptr, nullptr, 0);
    if (flush_err) {
        (void)fprintf(stderr, "problem flushing context\n");
        return flush_err;
    }
    if (!c->no_prediction) {
        const int score_err = report_pooled_scores(run.vmaf, c, run.arrays, loop.frames, istty);
        if (score_err)
            return score_err;
    }
    return write_cli_output(run);
}

[[nodiscard]] int execute_cli(CliRunState &run, int istty)
{
    int err = open_cli_inputs(run);
    if (err)
        return err;
    const int common_bitdepth = common_input_bitdepth(run);
    err = initialize_cli_context(run);
    if (err)
        return err;
    const VmafPictureConfiguration pic_cfg = make_picture_configuration(run, common_bitdepth);
    err = prepare_picture_pool(run, pic_cfg, istty);
    if (err)
        return err;
    err = load_cli_models(run, pic_cfg);
    if (err)
        return err;
    err = register_cli_features(run, pic_cfg);
    if (err)
        return err;
    return run_cli_frames(run, common_bitdepth, istty);
}

} // namespace

int main(int argc, char *argv[])
{
    const int istty = isatty(fileno(stderr));
#ifdef _WIN32
    /* cli_parse() exits directly for help/version/errors, so only static
     * storage guarantees restoration on both exit() and ordinary returns. */
    static const WindowsConsoleGuard console_guard;
#endif
    CliRunState run;
    cli_parse(argc, argv, &run.settings);
    print_version_banner(&run.settings, istty);
    const int result = execute_cli(run, istty);
    const int cleanup_result = cleanup_cli_run(&run);
    return result ? result : cleanup_result;
}
