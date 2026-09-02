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
 * model_collection_label) that previously required manual free() under the
 * goto-cleanup ladder.  The goto-cleanup spine is retained — it is a
 * load-bearing invariant per ADR-0141 §2 (cleanup ownership chain); jumping
 * over trivially-destructible or default-initialised objects is well-formed
 * C++23.  Spinner header uses inline to suppress ODR warnings. */

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
#else
    const int raw_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    FILE *fp = (raw_fd >= 0) ? fdopen(raw_fd, "wb") : nullptr;
    if (!fp && raw_fd >= 0)
        (void)close(raw_fd);
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

/* Copy video input data to picture buffer. The four bit-depth × component
 * branches (8-bit Y/U/V, 10-bit Y/U/V, 16-bit packed) duplicate the per-row
 * loop with different per-sample casts; folding them through a function
 * pointer would cost a per-row indirect call on every frame, so the
 * branches stay inline. The nesting-level warning is structural to YUV
 * (plane × row × column) — splitting wouldn't reduce it
 * (ADR-0141 §2 load-bearing invariant: per-frame indirect-call cost;
 * T7-5 sweep closeout — ADR-0278).
 */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — ADR-1155: per-frame pixel unpacking loop
void copy_picture_data(VmafPicture *pic, video_input_ycbcr ycbcr, video_input_info *info, int depth)
{
    if (info->depth == depth) {
        if (info->depth == 8) {
            for (unsigned i = 0; i < 3; i++) {
                const int xdec = i && !(info->pixel_fmt & 1);
                const int ydec = i && !(info->pixel_fmt & 2);
                const uint8_t *ycbcr_data =
                    ycbcr[i].data + static_cast<size_t>(info->pic_y >> ydec) * ycbcr[i].stride +
                    (info->pic_x >> xdec);
                uint8_t *pic_data = static_cast<uint8_t *>(pic->data[i]);

                for (unsigned j = 0; j < pic->h[i]; j++) {
                    memcpy(pic_data, ycbcr_data, sizeof(*pic_data) * pic->w[i]);
                    pic_data += pic->stride[i];
                    ycbcr_data += ycbcr[i].stride;
                }
            }
        } else {
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
    } else if (depth > 8) {
        // unequal bit-depth
        // therefore depth must be > 8 since we do not support depth < 8
        const int left_shift = depth - info->depth;
        if (info->depth == 8) {
            for (unsigned i = 0; i < 3; i++) {
                const int xdec = i && !(info->pixel_fmt & 1);
                const int ydec = i && !(info->pixel_fmt & 2);
                const uint8_t *ycbcr_data =
                    ycbcr[i].data + static_cast<size_t>(info->pic_y >> ydec) * ycbcr[i].stride +
                    (info->pic_x >> xdec);
                uint16_t *pic_data = static_cast<uint16_t *>(pic->data[i]);

                for (unsigned j = 0; j < pic->h[i]; j++) {
                    for (unsigned k = 0; k < pic->w[i]; k++) {
                        pic_data[k] = static_cast<uint16_t>(ycbcr_data[k] << left_shift);
                    }
                    pic_data += pic->stride[i] / 2;
                    ycbcr_data += ycbcr[i].stride;
                }
            }
        } else {
            for (unsigned i = 0; i < 3; i++) {
                const int xdec = i && !(info->pixel_fmt & 1);
                const int ydec = i && !(info->pixel_fmt & 2);
                const uint16_t *ycbcr_data =
                    reinterpret_cast<const uint16_t *>(ycbcr[i].data) +
                    static_cast<size_t>(info->pic_y >> ydec) * (ycbcr[i].stride / 2) +
                    (info->pic_x >> xdec);
                uint16_t *pic_data = static_cast<uint16_t *>(pic->data[i]);

                for (unsigned j = 0; j < pic->h[i]; j++) {
                    for (unsigned k = 0; k < pic->w[i]; k++) {
                        pic_data[k] = static_cast<uint16_t>(ycbcr_data[k] << left_shift);
                    }
                    pic_data += pic->stride[i] / 2;
                    ycbcr_data += ycbcr[i].stride / 2;
                }
            }
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
 * arrays sized to model_cnt.  The destructor calls vmaf_model_destroy /
 * vmaf_model_collection_destroy and frees the backing store so the
 * goto-cleanup spine in main() simply lets this object go out of scope.
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
    [[nodiscard]] unsigned &model_cnt() noexcept
    {
        return m_model_cnt;
    }
    [[nodiscard]] unsigned model_cnt() const noexcept
    {
        return m_model_cnt;
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
                                              ModelArrays &arrays)
{
    unsigned *slot = &arrays.collection_cnt();
    int err;

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
                                       ModelArrays &arrays)
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
        return load_model_collection_entry(vmaf, c, i, arrays);
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
 * for cleanup unwinding. Returns 0 on success, -1 on any failure (caller
 * should treat as fatal and `goto cleanup`).
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

/* Initialise the GPU backends in declared priority order: SYCL first
 * (preferred when --sycl_device or --gpumask is set), CUDA second
 * (consulted only if SYCL was not activated), then HIP and Metal
 * (explicit --hip_device / --metal_device opt-in). On a hard
 * backend-import failure returns -1 so the caller can `goto cleanup`;
 * soft init failures (state_init returning non-zero) silently fall
 * back to CPU. State pointers are passed by reference so the cleanup
 * block can free them after vmaf_close().
 *
 * The function is intentionally kept in a single TU even though
 * several #ifdef-guarded backend stanzas push the line count past the
 * 60-line threshold. Splitting into per-backend helpers would multiply
 * the `#if defined(HAVE_X)` decoration without making the activation
 * priority chain (SYCL > CUDA > HIP > Metal) any clearer to a reader
 * (ADR-0141 §2 load-bearing invariant: backend-priority chain
 * readability + #ifdef discipline; T7-5 sweep closeout — ADR-0278).
 */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — ADR-1155: backend priority chain and context init
[[nodiscard]] int init_gpu_backends(VmafContext *vmaf, const CLISettings *c
#ifdef HAVE_SYCL
                                    ,
                                    VmafSyclState **sycl_state, bool *sycl_active
#endif
#ifdef HAVE_CUDA
                                    ,
                                    bool *cuda_active_out
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
    int err;
    (void)vmaf;
    (void)c;
    (void)err;

    /* ADR-0498 / Bug #v2-E: when the user passes ``--backend NAME``
     * (not the default ``auto``), an init failure for the requested
     * backend must surface as a hard error — silently falling back to
     * CPU corrupts CI gates that depend on backend-specific scoring.
     * The ``auto`` selector keeps the legacy soft-fallback chain.
     * Marked (void) so a build with no GPU backends compiled in
     * doesn't trip ``-Wunused-variable``. */
    const bool explicit_backend =
        c->backend && strcmp(c->backend, "auto") != 0 && strcmp(c->backend, "cpu") != 0;
    (void)explicit_backend;

    /* If the requested backend isn't compiled into this libvmaf,
     * surface that as a hard error too — otherwise the CLI silently
     * runs on CPU and the user has no signal beyond stderr. */
    if (explicit_backend) {
        const bool compiled_in = false;
#ifdef HAVE_SYCL
        if (strcmp(c->backend, "sycl") == 0)
            compiled_in = true;
#endif
#ifdef HAVE_CUDA
        if (strcmp(c->backend, "cuda") == 0)
            compiled_in = true;
#endif
#ifdef HAVE_HIP
        if (strcmp(c->backend, "hip") == 0)
            compiled_in = true;
#endif
#ifdef HAVE_METAL
        if (strcmp(c->backend, "metal") == 0)
            compiled_in = true;
#endif
        if (!compiled_in) {
            (void)fprintf(stderr,
                          "vmaf: --backend %s requested but this libvmaf was built "
                          "without %s support; refusing to silently fall back to CPU "
                          "(ADR-0498)\n",
                          c->backend, c->backend);
            write_backend_error_json(c->output_path, c->output_fmt, c->backend,
                                     "backend not compiled into this libvmaf", 0);
            return VMAF_INIT_GPU_EXPLICIT_FAIL;
        }
    }

    // GPU backend initialization: each backend activates only when its
    // specific flag is passed.  --gpumask enables the preferred backend
    // (SYCL > CUDA).  --sycl_device selects
    // that specific backend.  No flag = CPU only.
#ifdef HAVE_SYCL
    VmafSyclConfiguration sycl_cfg = {
        .device_index = c->sycl_device >= 0 ? c->sycl_device : 0,
    };
    if ((c->sycl_device >= 0 || c->use_gpumask) && !c->no_sycl) {
        err = vmaf_sycl_state_init(sycl_state, sycl_cfg);
        if (err) {
            (void)fprintf(stderr, "problem during vmaf_sycl_state_init, using CPU\n");
            if (explicit_backend && strcmp(c->backend, "sycl") == 0) {
                (void)fprintf(stderr, "vmaf: --backend sycl requested but init failed; "
                                      "refusing to silently fall back to CPU (ADR-0498)\n");
                write_backend_error_json(c->output_path, c->output_fmt, "sycl",
                                         "vmaf_sycl_state_init failed", err);
                return VMAF_INIT_GPU_EXPLICIT_FAIL;
            }
        } else {
            err = vmaf_sycl_import_state(vmaf, *sycl_state);
            if (err) {
                (void)fprintf(stderr, "problem during vmaf_sycl_import_state\n");
                return -1;
            }
            *sycl_active = true;
        }
    }
#endif
#ifdef HAVE_CUDA
    *cuda_active_out = false;
    VmafCudaState *cu_state;
    VmafCudaConfiguration cuda_cfg = {0};
    if (c->use_gpumask && !c->no_cuda
#ifdef HAVE_SYCL
        && !*sycl_active
#endif
    ) {
        err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
        if (err) {
            (void)fprintf(stderr, "problem during vmaf_cuda_state_init, using CPU\n");
            if (explicit_backend && strcmp(c->backend, "cuda") == 0) {
                (void)fprintf(stderr, "vmaf: --backend cuda requested but init failed; "
                                      "refusing to silently fall back to CPU (ADR-0498)\n");
                write_backend_error_json(c->output_path, c->output_fmt, "cuda",
                                         "vmaf_cuda_state_init failed", err);
                return VMAF_INIT_GPU_EXPLICIT_FAIL;
            }
        } else {
            err |= vmaf_cuda_import_state(vmaf, cu_state);
            if (err) {
                (void)fprintf(stderr, "problem during vmaf_cuda_import_state\n");
                return -1;
            }
            *cuda_active_out = true;
        }
    }
#endif

#ifdef HAVE_HIP
    /* HIP opt-in: explicit --hip_device only. Same lifetime model as
     * SYCL — state is passed back by reference so the cleanup
     * block can free it after vmaf_close(). */
    VmafHipConfiguration hip_cfg = {
        .device_index = c->hip_device,
        .flags = 0,
    };
    if (c->hip_device >= 0 && !c->no_hip) {
        err = vmaf_hip_state_init(hip_state, hip_cfg);
        if (err) {
            (void)fprintf(stderr, "problem during vmaf_hip_state_init (%d), using CPU\n", err);
            if (explicit_backend && strcmp(c->backend, "hip") == 0) {
                (void)fprintf(stderr, "vmaf: --backend hip requested but init failed; "
                                      "refusing to silently fall back to CPU (ADR-0498)\n");
                write_backend_error_json(c->output_path, c->output_fmt, "hip",
                                         "vmaf_hip_state_init failed", err);
                return VMAF_INIT_GPU_EXPLICIT_FAIL;
            }
        } else {
            err = vmaf_hip_import_state(vmaf, *hip_state);
            if (err) {
                (void)fprintf(stderr, "problem during vmaf_hip_import_state\n");
                return -1;
            }
            *hip_active = true;
        }
    }
    (void)*hip_active;
#endif

#ifdef HAVE_METAL
    /* Metal opt-in: explicit --metal_device only. macOS-only; on non-
     * Apple hosts vmaf_metal_state_init returns -ENODEV and the CLI
     * falls back to CPU. Same state-lifetime model as SYCL/HIP. */
    VmafMetalConfiguration metal_cfg = {
        .device_index = c->metal_device,
        .flags = 0,
    };
    if (c->metal_device >= 0 && !c->no_metal) {
        err = vmaf_metal_state_init(metal_state, metal_cfg);
        if (err) {
            (void)fprintf(stderr, "problem during vmaf_metal_state_init (%d), using CPU\n", err);
            if (explicit_backend && strcmp(c->backend, "metal") == 0) {
                (void)fprintf(stderr, "vmaf: --backend metal requested but init failed; "
                                      "refusing to silently fall back to CPU (ADR-0498)\n");
                write_backend_error_json(c->output_path, c->output_fmt, "metal",
                                         "vmaf_metal_state_init failed", err);
                return VMAF_INIT_GPU_EXPLICIT_FAIL;
            }
        } else {
            err = vmaf_metal_import_state(vmaf, *metal_state);
            if (err) {
                (void)fprintf(stderr, "problem during vmaf_metal_import_state\n");
                return -1;
            }
            *metal_active = true;
        }
    }
    (void)*metal_active;
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
 * load and never touches ORT. Returns 0 on success, -1 on any failure
 * (caller should treat as fatal and `goto cleanup`).
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

/* Drive the main per-frame fetch + process loop. Returns the number of frames
 * successfully consumed (the post-increment `picture_index` value the original
 * inline loop used to compute `picture_index - 1` in pooling). Stops at EOF
 * on either side, on read errors, or when c->frame_cnt is reached.
 */
unsigned run_frame_loop(VmafContext *vmaf, video_input *vid_ref, video_input *vid_dist,
                        const CLISettings *c, int common_bitdepth, int istty)
{
    float fps = 0.;
    const double t0 = wall_time_s();
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

        if (ret1 && ret2) {
            break;
        } else if (ret1 < 0 || ret2 < 0) {
            (void)fprintf(stderr, "\nproblem while reading pictures\n");
            break;
        } else if (ret1) {
            (void)fprintf(stderr, "\n\"%s\" ended before \"%s\".\n", c->path_ref, c->path_dist);
            break;
        } else if (ret2) {
            (void)fprintf(stderr, "\n\"%s\" ended before \"%s\".\n", c->path_dist, c->path_ref);
            break;
        }

        if (istty && !c->quiet) {
            if (picture_index > 0 && !(picture_index % 10)) {
                fps = static_cast<float>((picture_index + 1) / (wall_time_s() - t0));
            }

            (void)fprintf(stderr, "\r%u frame%s %s %.2f FPS\033[K", picture_index + 1,
                          picture_index ? "s" : " ", spinner[picture_index % spinner_length], fps);
            (void)fflush(stderr);
        }

        const int err = vmaf_read_pictures(vmaf, &pic_ref, &pic_dist, picture_index);
        if (err) {
            (void)fprintf(stderr, "\nproblem reading pictures\n");
            break;
        }
    }
    if (istty && !c->quiet)
        (void)fprintf(stderr, "\n");

    return picture_index;
}

/* Compute and report pooled VMAF scores for all loaded models and model
 * collections. Called only when c->no_prediction is false. Returns 0 on
 * success, non-zero on the first per-model scoring failure (caller should
 * treat as fatal and `goto cleanup`).
 */
[[nodiscard]] int report_pooled_scores(VmafContext *vmaf, const CLISettings *c,
                                       const ModelArrays &arrays, unsigned picture_index, int istty)
{
    int err = 0;

    for (unsigned i = 0; i < c->model_cnt; i++) {
        double vmaf_score;
        err = vmaf_score_pooled(vmaf, arrays.model()[i], VMAF_POOL_METHOD_MEAN, &vmaf_score, 0,
                                picture_index - 1);
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
        err = vmaf_score_pooled_model_collection(
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
 */
void amend_json_with_backend_used(const char *output_path, enum VmafOutputFormat fmt,
                                  const char *backend_used)
{
    if (!output_path || !backend_used)
        return;
    if (fmt != VMAF_OUTPUT_FORMAT_JSON)
        return;

    FILE *fp = fopen(output_path, "rb+");
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

/* CLI driver: orchestrates input opening, VMAF context init, GPU backend
 * activation, model loading, frame loop, score reporting, and cleanup.
 * The function is structured around a single goto-cleanup block — a
 * load-bearing invariant per ADR-0141 §2 because each subsystem owns a
 * distinct cleanup primitive (fclose / video_input_close / vmaf_close /
 * vmaf_*_state_free / cli_free) that must run in reverse-init order on
 * every exit path.  The ModelArrays RAII object (ADR-0809) replaces the
 * three manual free/destroy loops that previously lived in the cleanup
 * block; the goto-cleanup spine is retained for the remaining resources.
 * T7-5 sweep extracted the eight largest sub-blocks into named helpers.
 * The remaining body is the cleanup-ownership spine plus inter-step glue;
 * further extraction would require pointer-aliasing the cleanup-relevant
 * locals through helper signatures and obscure the unwind chain
 * (ADR-0141 §2; T7-5 sweep closeout — ADR-0278; ADR-0146 prior precedent).
 */
} // namespace

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — ADR-1155: top-level CLI main entry point
int main(int argc, char *argv[])
{
    int err = 0;
    int ret = 0;
    const int istty = isatty(fileno(stderr));

    CLISettings c;
    cli_parse(argc, argv, &c);

    FILE *file_ref = nullptr;
    FILE *file_dist = nullptr;
    bool vid_ref_open = false;
    bool vid_dist_open = false;
    video_input vid_ref = {.vtbl = nullptr, .ctx = nullptr, .fin = nullptr};
    video_input vid_dist = {.vtbl = nullptr, .ctx = nullptr, .fin = nullptr};
    VmafContext *vmaf = nullptr;

    /* ModelArrays is trivially default-constructible (all pointers null,
     * counts zero) so jumping to `cleanup:` before allocate() is called
     * is well-formed C++23: the destructor runs but does nothing. */
    ModelArrays arrays;

#ifdef HAVE_SYCL
    bool sycl_active = false;
    VmafSyclState *sycl_state = nullptr;
#endif
#ifdef HAVE_CUDA
    /* ADR-0543: CUDA active flag is propagated out of init_gpu_backends
     * so the per-feature backend gate + `backend_used` JSON echo can
     * see it. Prior to ADR-0543 the flag was a local in init_gpu_backends
     * and the JSON echo inferred CUDA from the gpumask state — which
     * broke down when the user passed an explicit ``--feature *_cuda``
     * but no ``--backend cuda``. */
    bool cuda_active = false;
#endif
#ifdef HAVE_HIP
    bool hip_active = false;
    VmafHipState *hip_state = nullptr;
#endif
#ifdef HAVE_METAL
    bool metal_active = false;
    VmafMetalState *metal_state = nullptr;
#endif

    if (istty && !c.quiet) {
        (void)fprintf(stderr, "VMAF version %s\n", vmaf_version());
    }

    /* ADR-0520: --no-reference mode opens the distorted file twice and
     * threads both handles through the existing ref+dist code paths.
     * `vmaf_read_pictures` enforces a non-null picture pair (the public
     * API contract: either both NULL = flush, or both non-NULL = score),
     * so we satisfy it with two independent decoded copies of the same
     * source. The NR tiny-model dispatch in `vmaf_ctx_dnn_run_frame_nchw`
     * reads picture bytes exclusively from the `ref` slot, so the model
     * sees the distorted frame as intended. No classic SVM features are
     * registered in this mode (cli_parse.c forces `no_prediction = true`
     * when `no_reference` is set), so the second copy is touched only by
     * `vmaf_picture_unref` in the cleanup tail. */
    const char *const ref_open_path = c.no_reference ? c.path_dist : c.path_ref;
    file_ref = fopen(ref_open_path, "rb");
    if (!file_ref) {
        (void)fprintf(stderr, "could not open file: %s\n", ref_open_path);
        ret = -1;
        goto cleanup;
    }

    file_dist = fopen(c.path_dist, "rb");
    if (!file_dist) {
        (void)fprintf(stderr, "could not open file: %s\n", c.path_dist);
        ret = -1;
        goto cleanup;
    }

    if (open_input_videos(&c, &file_ref, &file_dist, &vid_ref, &vid_dist, &vid_ref_open,
                          &vid_dist_open)) {
        ret = -1;
        goto cleanup;
    }

    {
        int common_bitdepth;
        if (c.use_yuv) {
            common_bitdepth = static_cast<int>(c.bitdepth);
        } else {
            video_input_info info1;
            video_input_info info2;
            video_input_get_info(&vid_ref, &info1);
            video_input_get_info(&vid_dist, &info2);
            common_bitdepth = info1.depth > info2.depth ? info1.depth : info2.depth;
        }

        const VmafConfiguration cfg = {
            .log_level = VMAF_LOG_LEVEL_INFO,
            .n_threads = c.thread_cnt,
            .n_subsample = c.subsample,
            .cpumask = c.cpumask,
            .gpumask = c.gpumask,
        };

        err = vmaf_init(&vmaf, cfg);
        if (err) {
            (void)fprintf(stderr, "problem initializing VMAF context\n");
            ret = -1;
            goto cleanup;
        }

        {
            const int gpu_rc = init_gpu_backends(vmaf, &c
#ifdef HAVE_SYCL
                                                 ,
                                                 &sycl_state, &sycl_active
#endif
#ifdef HAVE_CUDA
                                                 ,
                                                 &cuda_active
#endif
#ifdef HAVE_HIP
                                                 ,
                                                 &hip_state, &hip_active
#endif
#ifdef HAVE_METAL
                                                 ,
                                                 &metal_state, &metal_active
#endif
            );
            if (gpu_rc) {
                /* ADR-0543 (extends ADR-0498): explicit-backend init failure
                 * must surface as a distinct non-zero exit code so CI gates
                 * (vmaf-tune bisect, MCP probes) can distinguish "you asked
                 * for SYCL and it isn't there" from a generic encode/score
                 * error. The init_gpu_backends helper returns the dedicated
                 * sentinel VMAF_INIT_GPU_EXPLICIT_FAIL for that case and the
                 * generic -1 for everything else (e.g. an import failure
                 * after a successful state_init). */
                ret = (gpu_rc == VMAF_INIT_GPU_EXPLICIT_FAIL) ? VMAF_EXIT_BACKEND_INIT_FAILED : -1;
                goto cleanup;
            }
        }

        // Preallocate picture pool to avoid allocation overhead
        video_input_info info;
        video_input_get_info(&vid_ref, &info);

        const VmafPictureConfiguration pic_cfg = {
            .pic_params =
                {
                    .w = static_cast<unsigned>(info.pic_w),
                    .h = static_cast<unsigned>(info.pic_h),
                    .bpc = static_cast<unsigned>(common_bitdepth),
                    .pix_fmt = pix_fmt_map(info.pixel_fmt),
                },
            /* Liveness budget per frame:
             *   2  — ref + dist currently held by the CLI fetch/process step
             *   1  — `vmaf->prev_ref` keeps the previous frame's ref picture
             *        live across the frame boundary (for motion features)
             *   2*thread_cnt — worker threads may hold (ref, dist) on in-flight
             *        frames that haven't finished processing yet
             * The `+ 1` term covers prev_ref uniformly. Undersizing deadlocks
             * vmaf_picture_pool_fetch on frame N+1. */
            .pic_cnt = 2 * (c.thread_cnt + 1) + 1,
        };

        err = vmaf_preallocate_pictures(vmaf, pic_cfg);
        if (err) {
            (void)fprintf(stderr, "problem during vmaf_preallocate_pictures\n");
            ret = -1;
            goto cleanup;
        }

        if (istty && !c.quiet) {
            (void)fprintf(stderr, "picture pool: %u pictures pre-allocated\n", pic_cfg.pic_cnt);
        }

        if (arrays.allocate(c.model_cnt)) {
            ret = -1;
            goto cleanup;
        }

        for (unsigned i = 0; i < c.model_cnt; i++) {
            if (load_one_model_entry(vmaf, &c, i, arrays)) {
                ret = -1;
                goto cleanup;
            }
        }

        /* ADR-0543 (extends ADR-0498): a feature name ending in ``_cuda`` /
         * ``_sycl`` / ``_hip`` / ``_metal`` is a GPU-pinned
         * variant. If the matching backend isn't active in this run, the
         * libvmaf feature registry silently registers the CPU twin and the
         * resulting scores look identical to an explicit-backend invocation
         * but were actually computed on the CPU — that's exactly the kind
         * of silent-fallback bug ADR-0498 banned for ``--backend NAME``.
         * Apply the same hard-fail policy here: surface a clear error +
         * write the structured JSON descriptor + exit with the dedicated
         * VMAF_EXIT_BACKEND_INIT_FAILED code. */
        for (unsigned i = 0; i < c.feature_cnt; i++) {
            const char *requested_be = nullptr;
            if (feature_backend_suffix(c.feature_cfg[i].name, &requested_be)) {
                const bool sa =
#ifdef HAVE_SYCL
                    sycl_active;
#else
                    false;
#endif
                const bool ca =
#ifdef HAVE_CUDA
                    cuda_active;
#else
                    false;
#endif
                const bool ha =
#ifdef HAVE_HIP
                    hip_active;
#else
                    false;
#endif
                const bool ma =
#ifdef HAVE_METAL
                    metal_active;
#else
                    false;
#endif
                if (!backend_active(requested_be, sa, ca, ha, ma)) {
                    (void)fprintf(stderr,
                                  "vmaf: --feature %s pinned to %s backend but %s is not "
                                  "active in this run; refusing to silently fall back to CPU "
                                  "(ADR-0498)\n",
                                  c.feature_cfg[i].name, requested_be, requested_be);
                    write_backend_error_json(c.output_path, c.output_fmt, requested_be,
                                             "feature pinned to inactive backend", 0);
                    ret = VMAF_EXIT_BACKEND_INIT_FAILED;
                    goto cleanup;
                }
            }
            err = vmaf_use_feature(vmaf, c.feature_cfg[i].name, c.feature_cfg[i].opts_dict);
            if (err) {
                (void)fprintf(stderr, "problem loading feature extractor: %s\n",
                              c.feature_cfg[i].name);
                ret = -1;
                goto cleanup;
            }
        }

        if (configure_tiny_model(vmaf, &c)) {
            ret = -1;
            goto cleanup;
        }

        skip_initial_frames(vmaf, &vid_ref, &vid_dist, &c, common_bitdepth);

        const unsigned picture_index =
            run_frame_loop(vmaf, &vid_ref, &vid_dist, &c, common_bitdepth, istty);

        /* No-frames guard: the pooling path below computes `picture_index - 1`.
         * With zero decoded frames that underflows the unsigned counter to
         * UINT_MAX and feeds a garbage index range into vmaf_score_pooled.
         * Fail fast with a dedicated exit code instead. */
        if (picture_index == 0) {
            (void)fprintf(stderr, "no frames decoded from \"%s\" / \"%s\"\n", c.path_ref,
                          c.path_dist);
            ret = VMAF_EXIT_NO_FRAMES_DECODED;
            goto cleanup;
        }

        err |= vmaf_read_pictures(vmaf, nullptr, nullptr, 0);
        if (err) {
            (void)fprintf(stderr, "problem flushing context\n");
            ret = err;
            goto cleanup;
        }

        if (!c.no_prediction) {
            ret = report_pooled_scores(vmaf, &c, arrays, picture_index, istty);
            if (ret)
                goto cleanup;
        }

        if (c.output_path) {
            const int write_err =
                vmaf_write_output_with_format(vmaf, c.output_path, c.output_fmt, c.precision_fmt);
            if (write_err) {
                /* A discarded return here hid write failures (bad path,
                 * ENOSPC, permission denied) behind a clean exit 0 over a
                 * stale or partial output file. Surface it instead. */
                (void)fprintf(stderr, "problem writing output to %s (err=%d)\n", c.output_path,
                              write_err);
                ret = write_err;
                goto cleanup;
            }
            /* ADR-0498 / Bug #v2-E: echo the active backend into the JSON
             * output so CI gates and MCP probes can confirm what actually
             * ran (mirrors the MCP-layer echo added by PR #1251). */
            const char *backend_used = "cpu";
#ifdef HAVE_SYCL
            if (sycl_active)
                backend_used = "sycl";
#endif
#ifdef HAVE_HIP
            if (hip_active)
                backend_used = "hip";
#endif
#ifdef HAVE_METAL
            if (metal_active)
                backend_used = "metal";
#endif
#ifdef HAVE_CUDA
            /* ADR-0543: CUDA's active flag is now propagated out of
             * init_gpu_backends so we can echo it directly instead of
             * re-deriving it from the gpumask + no-flags state. */
            if (cuda_active)
                backend_used = "cuda";
#endif
            amend_json_with_backend_used(c.output_path, c.output_fmt, backend_used);
        }

        ret = err;
    }

cleanup:
    /* ModelArrays destructor handles model/collection teardown automatically
     * (ADR-0809 RAII).  GPU states and video inputs follow below. */
    if (vmaf)
        vmaf_close(vmaf);
#ifdef HAVE_SYCL
    if (sycl_active)
        vmaf_sycl_state_free(&sycl_state);
#endif
#ifdef HAVE_HIP
    if (hip_state)
        vmaf_hip_state_free(&hip_state);
#endif
#ifdef HAVE_METAL
    if (metal_state)
        vmaf_metal_state_free(&metal_state);
#endif
    if (vid_dist_open)
        video_input_close(&vid_dist);
    if (vid_ref_open)
        video_input_close(&vid_ref);
    if (file_dist)
        (void)fclose(file_dist);
    if (file_ref)
        (void)fclose(file_ref);
    cli_free(&c);
    return ret;
}
