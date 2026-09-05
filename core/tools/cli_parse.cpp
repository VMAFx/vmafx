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

/* ADR-0809 — C++23 Wave 8: cli_parse.c → cli_parse.cpp.
 * Conservative idioms only: nullptr, static_cast, std::string_view for
 * option-string comparisons (removes pointer arithmetic / strlen calls),
 * [[nodiscard]] on helpers that return error-coded values.
 * The public ABI (cli_parse.h) is unchanged; extern "C" guards added to
 * that header keep all C callers compiling without modification. */

#include "config.h"

#ifdef _WIN32
#include "compat/win32/getopt.h"
#include <windows.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "cli_parse.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

namespace
{

const char short_opts[] = "r:d:w:h:p:b:m:c:o:nvq";

enum : std::uint16_t {
    ARG_OUTPUT_XML = 256,
    ARG_OUTPUT_JSON,
    ARG_OUTPUT_CSV,
    ARG_OUTPUT_SUB,
    ARG_THREADS,
    ARG_FEATURE,
    ARG_SUBSAMPLE,
    ARG_HELP,
    ARG_CPUMASK,
    ARG_GPUMASK,
    ARG_AOM_CTC,
    ARG_NFLX_CTC,
    ARG_FRAME_CNT,
    ARG_FRAME_SKIP_REF,
    ARG_FRAME_SKIP_DIST,
    ARG_NO_CUDA,
    ARG_NO_SYCL,
    ARG_SYCL_DEVICE,
    ARG_NO_HIP,
    ARG_HIP_DEVICE,
    ARG_NO_METAL,
    ARG_METAL_DEVICE,
    ARG_BACKEND,
    ARG_PRECISION,
    ARG_TINY_MODEL,
    ARG_TINY_DEVICE,
    ARG_TINY_THREADS,
    ARG_TINY_FP16,
    ARG_TINY_MODEL_VERIFY,
    /* ADR-0519 — codec context for codec-aware tiny models
     * (e.g. fr_regressor_v2). All three default unset. */
    ARG_TINY_CODEC,
    ARG_TINY_PRESET,
    ARG_TINY_CRF,
    ARG_NO_REFERENCE,
    ARG_DNN_EP,
    /* ADR-0550 — NCHW tiny-model auto-resize filter. */
    ARG_TINY_RESIZE,
    /* ADR-0696 — restore Netflix-upstream legacy defaults. */
    ARG_NETFLIX_COMPAT,
};

/* Default matches Netflix's pre-fork output exactly so the CPU golden
 * gate passes without explicit flags (CLAUDE.md §8). Round-trip lossless
 * formatting is opt-in via --precision=max. See ADR-0119 (supersedes
 * ADR-0006). */
#define VMAF_DEFAULT_PRECISION_FMT "%.6f"
#define VMAF_LOSSLESS_PRECISION_FMT "%.17g"

char precision_fmt_buf[16];

const char *resolve_precision_fmt(const char *optarg, const char *app, CLISettings *s)
{
    using sv = std::string_view;
    const sv arg{optarg};
    if (arg == "max" || arg == "full") {
        s->precision_max = true;
        return VMAF_LOSSLESS_PRECISION_FMT;
    }
    if (arg == "legacy") {
        /* `legacy` is now the default; keep the alias accepted so existing
         * scripts that pass it explicitly do not break. */
        s->precision_legacy = true;
        return VMAF_DEFAULT_PRECISION_FMT;
    }
    char *end = nullptr;
    const long n = strtol(optarg, &end, 10);
    if (*end || end == optarg || n < 1 || n > 17) {
        (void)fprintf(stderr,
                      "%s: --precision must be an integer 1..17, "
                      "or one of: max, full, legacy (got: %s)\n",
                      app, optarg);
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: CLI error exit
        exit(1);
    }
    s->precision_n = static_cast<int>(n);
    (void)snprintf(precision_fmt_buf, sizeof precision_fmt_buf, "%%.%ldg", n);
    return precision_fmt_buf;
}

const struct option long_opts[] = {
    {.name = "reference", .has_arg = 1, .flag = nullptr, .val = 'r'},
    {.name = "distorted", .has_arg = 1, .flag = nullptr, .val = 'd'},
    {.name = "width", .has_arg = 1, .flag = nullptr, .val = 'w'},
    {.name = "height", .has_arg = 1, .flag = nullptr, .val = 'h'},
    {.name = "pixel_format", .has_arg = 1, .flag = nullptr, .val = 'p'},
    {.name = "bitdepth", .has_arg = 1, .flag = nullptr, .val = 'b'},
    {.name = "model", .has_arg = 1, .flag = nullptr, .val = 'm'},
    {.name = "output", .has_arg = 1, .flag = nullptr, .val = 'o'},
    {.name = "xml", .has_arg = 0, .flag = nullptr, .val = ARG_OUTPUT_XML},
    {.name = "json", .has_arg = 0, .flag = nullptr, .val = ARG_OUTPUT_JSON},
    {.name = "csv", .has_arg = 0, .flag = nullptr, .val = ARG_OUTPUT_CSV},
    {.name = "sub", .has_arg = 0, .flag = nullptr, .val = ARG_OUTPUT_SUB},
    {.name = "help", .has_arg = 0, .flag = nullptr, .val = ARG_HELP},
    {.name = "threads", .has_arg = 1, .flag = nullptr, .val = ARG_THREADS},
    {.name = "feature", .has_arg = 1, .flag = nullptr, .val = ARG_FEATURE},
    {.name = "subsample", .has_arg = 1, .flag = nullptr, .val = ARG_SUBSAMPLE},
    {.name = "cpumask", .has_arg = 1, .flag = nullptr, .val = ARG_CPUMASK},
    {.name = "gpumask", .has_arg = 1, .flag = nullptr, .val = ARG_GPUMASK},
    {.name = "aom_ctc", .has_arg = 1, .flag = nullptr, .val = ARG_AOM_CTC},
    {.name = "nflx_ctc", .has_arg = 1, .flag = nullptr, .val = ARG_NFLX_CTC},
    {.name = "frame_cnt", .has_arg = 1, .flag = nullptr, .val = ARG_FRAME_CNT},
    {.name = "frame_skip_ref", .has_arg = 1, .flag = nullptr, .val = ARG_FRAME_SKIP_REF},
    {.name = "frame_skip_dist", .has_arg = 1, .flag = nullptr, .val = ARG_FRAME_SKIP_DIST},
    {.name = "no_cuda", .has_arg = 0, .flag = nullptr, .val = ARG_NO_CUDA},
    {.name = "no_sycl", .has_arg = 0, .flag = nullptr, .val = ARG_NO_SYCL},
    {.name = "sycl_device", .has_arg = 1, .flag = nullptr, .val = ARG_SYCL_DEVICE},
    {.name = "no_hip", .has_arg = 0, .flag = nullptr, .val = ARG_NO_HIP},
    {.name = "hip_device", .has_arg = 1, .flag = nullptr, .val = ARG_HIP_DEVICE},
    {.name = "no_metal", .has_arg = 0, .flag = nullptr, .val = ARG_NO_METAL},
    {.name = "metal_device", .has_arg = 1, .flag = nullptr, .val = ARG_METAL_DEVICE},
    {.name = "backend", .has_arg = 1, .flag = nullptr, .val = ARG_BACKEND},
    {.name = "precision", .has_arg = 1, .flag = nullptr, .val = ARG_PRECISION},
    {.name = "tiny-model", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_MODEL},
    {.name = "tiny_model", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_MODEL},
    {.name = "tiny-device", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_DEVICE},
    {.name = "tiny_device", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_DEVICE},
    {.name = "tiny-threads", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_THREADS},
    {.name = "tiny_threads", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_THREADS},
    {.name = "tiny-fp16", .has_arg = 0, .flag = nullptr, .val = ARG_TINY_FP16},
    {.name = "tiny_fp16", .has_arg = 0, .flag = nullptr, .val = ARG_TINY_FP16},
    {.name = "tiny-model-verify", .has_arg = 0, .flag = nullptr, .val = ARG_TINY_MODEL_VERIFY},
    {.name = "tiny_model_verify", .has_arg = 0, .flag = nullptr, .val = ARG_TINY_MODEL_VERIFY},
    /* ADR-0519 — codec context. Underscore aliases match the rest
     * of the tiny-* family for scripting consistency. */
    {.name = "tiny-codec", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_CODEC},
    {.name = "tiny_codec", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_CODEC},
    {.name = "tiny-preset", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_PRESET},
    {.name = "tiny_preset", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_PRESET},
    {.name = "tiny-crf", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_CRF},
    {.name = "tiny_crf", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_CRF},
    {.name = "no-reference", .has_arg = 0, .flag = nullptr, .val = ARG_NO_REFERENCE},
    {.name = "no_reference", .has_arg = 0, .flag = nullptr, .val = ARG_NO_REFERENCE},
    /* ADR-0550 — NCHW tiny-model auto-resize filter selector. */
    {.name = "tiny-resize", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_RESIZE},
    {.name = "tiny_resize", .has_arg = 1, .flag = nullptr, .val = ARG_TINY_RESIZE},
    /* --dnn-ep is the user-facing name for selecting the ONNX Runtime
     * execution provider. It is an alias for --tiny-device so both flags
     * write to the same CLISettings.tiny_device field. Accepting both names
     * lets users follow the ORT "execution provider" terminology directly
     * without knowing the fork's internal "tiny-device" naming. */
    {.name = "dnn-ep", .has_arg = 1, .flag = nullptr, .val = ARG_DNN_EP},
    {.name = "dnn_ep", .has_arg = 1, .flag = nullptr, .val = ARG_DNN_EP},
    {.name = "no_prediction", .has_arg = 0, .flag = nullptr, .val = 'n'},
    {.name = "netflix-compat", .has_arg = 0, .flag = nullptr, .val = ARG_NETFLIX_COMPAT},
    {.name = "netflix_compat", .has_arg = 0, .flag = nullptr, .val = ARG_NETFLIX_COMPAT},
    {.name = "version", .has_arg = 0, .flag = nullptr, .val = 'v'},
    {.name = "quiet", .has_arg = 0, .flag = nullptr, .val = 'q'},
    {.name = nullptr, .has_arg = 0, .flag = nullptr, .val = 0},
};

void print_usage_options_part1(FILE *const out, const char *const app)
{
    (void)fprintf(out, "Usage: %s [options]\n\n", app);
    (void)fprintf(out, "Supported options:\n"
                       " --help:                      print this message and exit\n"
                       " --reference/-r $path:        path to reference .y4m or .yuv\n"
                       " --distorted/-d $path:        path to distorted .y4m or .yuv\n"
                       " --width/-w $unsigned:        width\n"
                       " --height/-h $unsigned:       height\n"
                       " --pixel_format/-p: $string   pixel format (420/422/444)\n"
                       " --bitdepth/-b $unsigned:     bitdepth (8/10/12/16)\n"
                       " --model/-m $params:          model parameters, colon \":\" delimited\n"
                       "                              `path=` path to model file\n"
                       "                              `version=` built-in model version\n"
                       "                              `name=` name used in log (optional)\n"
                       " --output/-o $path:           output file\n"
                       " --xml:                       write output file as XML (default)\n"
                       " --json:                      write output file as JSON\n"
                       " --csv:                       write output file as CSV\n"
                       " --sub:                       write output file as subtitle\n"
                       " --threads $unsigned:         number of threads to use\n"
                       " --feature $string:           additional feature\n"
                       " --cpumask: $bitmask          restrict permitted CPU instruction sets\n"
                       " --gpumask: $bitmask          restrict permitted GPU operations\n"
                       " --frame_cnt $unsigned:       maximum number of frames to process\n"
                       " --frame_skip_ref $unsigned:  skip the first N frames in reference\n"
                       " --frame_skip_dist $unsigned: skip the first N frames in distorted\n"
                       " --subsample: $unsigned       compute scores only every N frames\n"
                       " --no_cuda:                   disable CUDA backend\n"
                       " --no_sycl:                    disable SYCL/oneAPI backend\n"
                       " --sycl_device $unsigned:      select SYCL GPU by index (default: auto)\n"
                       "                              [Vulkan backend removed in ADR-0726]\n");
}

void print_usage_options_part2(FILE *const out)
{
    (void)fprintf(
        out,
        " --no_hip:                     disable HIP (AMD ROCm) backend\n"
        " --hip_device $unsigned:       select HIP GPU by index (default: auto)\n"
        " --no_metal:                   disable Metal (Apple Silicon) backend\n"
        " --metal_device $unsigned:     select Metal GPU by index (default: auto)\n"
        " --backend $name:              exclusive backend selector — auto|cpu|cuda|sycl|hip|metal.\n"
        "                               When set to a specific backend, the others are\n"
        "                               disabled to avoid the dispatcher first-match-wins\n"
        "                               race. (Vulkan backend was removed in ADR-0726.)\n"
        " --precision $spec:            score output precision\n"
        "                                  N (1..17) -> printf \"%%.<N>g\"\n"
        "                                  max|full  -> \"%%.17g\" (round-trip lossless)\n"
        "                                  legacy    -> \"%%.6f\" (default; Netflix-compatible)\n"
        " --tiny-model $path:           load a tiny ONNX model alongside classic models\n"
        " --tiny-device $string:        auto|cpu|cuda|openvino|openvino-npu|\n"
        "                                  openvino-cpu|openvino-gpu|coreml|\n"
        "                                  coreml-ane|coreml-gpu|coreml-cpu|rocm\n"
        "                                  (default: auto)\n"
        " --dnn-ep $string:             alias for --tiny-device; selects the ONNX Runtime\n"
        "                                  execution provider by its ORT name\n"
        " --tiny-threads $unsigned:     CPU EP intra-op threads (0 = ORT default)\n"
        " --tiny-fp16:                  request fp16 IO where the EP supports it\n"
        " --tiny-model-verify:          require Sigstore-bundle verification (cosign verify-blob)\n"
        "                               of the loaded tiny model before use; refuses to load\n"
        "                               on missing bundle, missing cosign, or non-zero exit\n"
        " --tiny-codec $name:           encoder name for codec-aware tiny models\n"
        "                               (e.g. fr_regressor_v2). Must match the model's\n"
        "                               sidecar encoder_vocab (libx264|libx265|libsvtav1|\n"
        "                               libvvenc|libvpx-vp9|h264_nvenc|hevc_nvenc|\n"
        "                               av1_nvenc|h264_qsv|hevc_qsv|av1_qsv|unknown).\n"
        "                               Common ffprobe aliases (h264|hevc|av1|vp9|vvc)\n"
        "                               are accepted. Unknown names are rejected\n"
        "                               at attach time. Default: \"unknown\"\n"
        " --tiny-preset $string:        encoder preset string (medium|slow|p4|5|...);\n"
        "                               interpretation is encoder-specific and mirrors\n"
        "                               train_fr_regressor_v2.py::PRESET_ORDINAL.\n"
        "                               Default: ordinal 5 (medium-equivalent)\n"
        " --tiny-crf $unsigned:         CRF / QP integer used during encoding; clamped\n"
        "                               to [0, 63] and normalised by 63. Default: 0\n"
        " --no-reference:               no-reference mode; valid only with an NR tiny model\n"
        " --tiny-resize $string:        enable auto-resize for NCHW tiny models when the\n"
        "                               input frame dims don't match the model's expected\n"
        "                               shape (e.g. 576x324 input -> 224x224 nr_metric_v1).\n"
        "                               One of: bilinear, nearest, bicubic, disabled.\n"
        "                               Default: disabled (mismatch -> -ERANGE hard-error;\n"
        "                               operator must opt in to resize explicitly).\n"
        "                               Warning: bilinear/nearest/bicubic produce scores\n"
        "                               that differ by ~2%% on the same input -- document\n"
        "                               the filter alongside your model checkpoint.\n"
        " --quiet/-q:                  disable FPS meter when run in a TTY\n"
        " --no_prediction/-n:          no prediction, extract features only\n"
        " --netflix-compat:             restore Netflix-upstream legacy defaults (CPU backend,\n"
        "                                  %%.6f precision, v0.6.1 default model)\n"
        " --version/-v:                print version and exit\n");
}

[[noreturn]] void usage_exit(bool is_error)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: CLI usage exit
    exit(is_error ? 1 : 0);
}

[[noreturn]] void usage(const char *const app, const char *const reason)
{
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        (void)fputs(reason, stderr);
        (void)fprintf(stderr, "\n\n");
    }
    print_usage_options_part1(out, app);
    print_usage_options_part2(out);
    usage_exit(reason != nullptr);
}

template <typename First, typename... Rest>
[[noreturn]] void usage(const char *const app, const char *const reason, First &&first,
                        Rest &&...rest)
{
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        /* cppcheck-suppress wrongPrintfScanfArgNum ; `reason` is a runtime format
         * string and the arguments arrive as a template parameter pack, so
         * cppcheck cannot pair them: it resolves neither the conversion count in
         * `reason` nor `sizeof...(Rest)`, and reports a fixed mismatch. Every
         * call site in this file passes a string literal whose conversions match
         * its arguments, and the compiler checks those at the call site. This
         * overload exists precisely because it is only ever selected when at
         * least one argument is present -- the zero-argument case is the
         * non-template overload above, which uses fputs and never treats
         * `reason` as a format string. */
        (void)fprintf(stderr, reason, std::forward<First>(first), std::forward<Rest>(rest)...);
        (void)fprintf(stderr, "\n\n");
    }
    print_usage_options_part1(out, app);
    print_usage_options_part2(out);
    usage_exit(reason != nullptr);
}

[[noreturn]] void usage(const char *const app, std::nullptr_t)
{
    print_usage_options_part1(stdout, app);
    print_usage_options_part2(stdout);
    usage_exit(false);
}

template <typename T>
void checked_append(T *const arr, unsigned &cnt, const T &val, const char *const app,
                    const char *const desc)
{
    if (cnt == CLI_SETTINGS_STATIC_ARRAY_LEN) {
        usage(app, "A maximum of %d %s are supported\n", CLI_SETTINGS_STATIC_ARRAY_LEN, desc);
    }
    arr[cnt++] = val;
}

void checked_replace_feature(CLIFeatureConfig *const arr, unsigned &cnt,
                             const CLIFeatureConfig &val, const char *const app,
                             const char *const desc)
{
    unsigned i = 0;
    for (i = 0; i < cnt; i++) {
        if (!strcmp(arr[i].name, val.name)) {
            free(arr[i].buf);
            vmaf_feature_dictionary_free(&arr[i].opts_dict);
            arr[i] = val;
            break;
        }
    }
    if (i == cnt) {
        checked_append(arr, cnt, val, app, desc);
    }
}

void error(const char *const app, const char *const optarg, const int option,
           const char *const shouldbe)
{
    char optname[256];
    int n = 0;

    for (n = 0; long_opts[n].name; n++) {
        if (long_opts[n].val == option)
            break;
    }
    /* Replace assert(long_opts[n].name) with an explicit check: the
     * banned-function audit (ADR-0523) found that assert() here (a)
     * uses a banned macro per docs/principles.md §1.2 rule 30 in
     * analysis and (b) with GCC 16 + -O3 -flto the assert→SIGABRT path
     * was transformed into SIGSEGV via sprintf(NULL) on the next line,
     * making the failure non-deterministic and harder to diagnose.
     * Replace with a clean error-exit that never invokes UB. */
    if (!long_opts[n].name) {
        usage(app,
              "Invalid argument \"%s\" for unrecognised option (internal error: "
              "option code %d not in long_opts[])",
              optarg, option);
        return; /* unreachable — usage() calls exit(1); satisfies [[noreturn]] analysis */
    }
    if (long_opts[n].val < 256) {
        (void)snprintf(optname, sizeof(optname), "-%c/--%s", long_opts[n].val, long_opts[n].name);
    } else {
        (void)snprintf(optname, sizeof(optname), "--%s", long_opts[n].name);
    }

    usage(app, "Invalid argument \"%s\" for option %s; should be %s", optarg, optname, shouldbe);
}

[[nodiscard]] unsigned parse_unsigned(const char *const optarg, const int option,
                                      const char *const app)
{
    /* Reject negative strings before calling strtoul: POSIX strtoul silently
     * converts "-1" to ULONG_MAX via unsigned wrapping, which would then be
     * truncated to UINT_MAX and silently accepted. */
    if (optarg[0] == '-')
        error(app, optarg, option, "a non-negative integer");
    char *end = nullptr;
    errno = 0;
    const unsigned long ul = strtoul(optarg, &end, 0);
    if (*end || end == optarg || errno == ERANGE || ul > UINT_MAX)
        error(app, optarg, option, "an integer in [0, 2^32-1]");
    return static_cast<unsigned>(ul);
}

[[nodiscard]] unsigned parse_bitdepth(const char *const optarg, const int option,
                                      const char *const app)
{
    const unsigned bitdepth = parse_unsigned(optarg, option, app);
    if (!((bitdepth == 8) || (bitdepth == 10) || (bitdepth == 12) || (bitdepth == 16)))
        error(app, optarg, option, "a valid bitdepth (8/10/12/16)");
    return bitdepth;
}

[[nodiscard]] enum VmafPixelFormat parse_pix_fmt(const char *const optarg, const int option,
                                                 const char *const app)
{
    using sv = std::string_view;
    const sv arg{optarg};
    enum VmafPixelFormat pix_fmt = VMAF_PIX_FMT_UNKNOWN;

    if (arg == "420")
        pix_fmt = VMAF_PIX_FMT_YUV420P;
    if (arg == "422")
        pix_fmt = VMAF_PIX_FMT_YUV422P;
    if (arg == "444")
        pix_fmt = VMAF_PIX_FMT_YUV444P;

    if (!pix_fmt) {
        error(app, optarg, option,
              "a valid pixel format "
              "(420/422/444)");
    }

    return pix_fmt;
}

/* Renamed from `strsep`: MSVC's UCRT declares `strsep` as `extern "C"`, so an
 * anonymous-namespace definition of that name does not hide the declaration and
 * every call bound to the undecorated CRT symbol instead, failing the link with
 * "unresolved external symbol strsep referenced in function main". The
 * pre-rework code used a file-scope `static` redeclaration, which did hide it.
 * A distinct name cannot collide on any platform. HAVE_STRSEP still selects the
 * platform implementation where one exists. */
#ifndef HAVE_STRSEP
char *vmaf_cli_strsep(char **sp, const char *sep)
{
    char *p = nullptr;
    char *s = nullptr;
    if (!sp || !*sp || !**sp)
        return nullptr;
    s = *sp;
    p = s + strcspn(s, sep);
    if (*p != '\0')
        *p++ = '\0';
    *sp = p;
    return s;
}
#else
/* The platform provides strsep; forward to it so the call sites stay uniform. */
char *vmaf_cli_strsep(char **sp, const char *sep)
{
    return strsep(sp, sep);
}
#endif

/* ADR-1180: In-place escape-aware splitter for CLI option strings.
 * Scans *sp up to the first unescaped `sep`, compacts escaped delimiters
 * (`\<sep>`) and escaped backslashes (`\\`) in place, terminates the token
 * with '\0', updates *sp to point after the delimiter (or nullptr if end of
 * string), and returns the token start. */
char *vmaf_cli_split(char **sp, char sep)
{
    if (!sp || !*sp)
        return nullptr;
    if (!**sp) {
        *sp = nullptr;
        return nullptr;
    }

    char *head = *sp;
    char *read = head;
    char *write = head;

    for (size_t i = 0; i < 4096 && *read != '\0'; ++i) {
        if (*read == '\\') {
            if (*(read + 1) == sep) {
                *write++ = sep;
                read += 2;
            } else if (*(read + 1) == '\\') {
                *write++ = '\\';
                read += 2;
            } else {
                *write++ = *read++;
            }
        } else if (*read == sep) {
            *write = '\0';
            *sp = read + 1;
            return head;
        } else {
            *write++ = *read++;
        }
    }

    *write = '\0';
    *sp = nullptr;
    return head;
}

void apply_model_opt(CLIModelConfig &model_cfg, char *key, char *val, const char *const app)
{
    if (!strcmp(key, "path")) {
        model_cfg.path = val;
    } else if (!strcmp(key, "name")) {
        model_cfg.cfg.name = val;
    } else if (!strcmp(key, "version")) {
        model_cfg.version = val;
    } else if (!strcmp(key, "disable_clip")) {
        model_cfg.cfg.flags |= !strcmp(val, "true") ? VMAF_MODEL_FLAG_DISABLE_CLIP : 0;
    } else if (!strcmp(key, "enable_transform")) {
        model_cfg.cfg.flags |= !strcmp(val, "true") ? VMAF_MODEL_FLAG_ENABLE_TRANSFORM : 0;
    } else {
        if (model_cfg.overload_cnt == CLI_SETTINGS_STATIC_ARRAY_LEN) {
            usage(app,
                  "A maximum of %d feature overloads per model"
                  " are supported\n",
                  CLI_SETTINGS_STATIC_ARRAY_LEN);
        }
        char *name = vmaf_cli_split(&key, '.');
        model_cfg.feature_overload[model_cfg.overload_cnt].name = name;
        const char *const opt = vmaf_cli_split(&key, '.');
        const int err = vmaf_feature_dictionary_set(
            &model_cfg.feature_overload[model_cfg.overload_cnt].opts_dict, opt, val);
        if (err)
            usage(app, "Problem parsing model: \"%s\"\n", name);

        model_cfg.overload_cnt++;
    }
}

CLIModelConfig parse_model_config(const char *const optarg, const char *const app)
{
    const size_t optarg_sz = strnlen(optarg, 1024);
    char *optarg_copy = static_cast<char *>(malloc(optarg_sz + 1));
    if (!optarg_copy)
        usage(app, "error while parsing model option: %s", optarg);
    (void)memset(optarg_copy, 0, optarg_sz + 1);
    (void)strncpy(optarg_copy, optarg, optarg_sz);

    CLIModelConfig model_cfg = {
        .path = nullptr,
        .version = nullptr,
        .cfg =
            {
                .name = "vmaf",
                .flags = VMAF_MODEL_FLAGS_DEFAULT,
            },
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = optarg_copy,
        .is_default = false,
    };

    char *key_val = nullptr;
    while ((key_val = vmaf_cli_split(&optarg_copy, ':')) != nullptr) {
        char *key = vmaf_cli_split(&key_val, '=');
        char *val = vmaf_cli_split(&key_val, '=');
        if (key && !strcmp(key, "path") && val &&
            std::isalpha(static_cast<unsigned char>(val[0])) && val[1] == '\0' && optarg_copy &&
            (*optarg_copy == '\\' || *optarg_copy == '/')) {
            val[1] = ':';
            char *rest = vmaf_cli_split(&optarg_copy, ':');
            (void)rest;
            /* In-place compact any \= in the path value since it is not split by '=' again */
            char *r = val + 2;
            char *w = val + 2;
            for (size_t i = 0; i < 4096 && *r != '\0'; ++i) {
                if (*r == '\\' && *(r + 1) == '=') {
                    *w++ = '=';
                    r += 2;
                } else {
                    *w++ = *r++;
                }
            }
            *w = '\0';
        }
        if (!val) {
            if (!strcmp(key, "disable_clip") || !strcmp(key, "enable_transform")) {
                val = const_cast<char *>("true");
            } else {
                usage(app,
                      "Problem parsing model, "
                      "bad option string \"%s\".",
                      key);
            }
        }
        apply_model_opt(model_cfg, key, val, app);
    }

    return model_cfg;
}

/* CLI alias map: user-facing "integer_*" names to the internal extractor
 * registration names.  Extractors register without the "integer_" prefix (or
 * with a completely different name), so passing the prefix verbatim yields
 * "problem loading feature extractor".  Adding the map here — at the parse
 * layer — keeps the rewrite in one place and leaves the extractor registry
 * unchanged.  See the commit that introduced this table for the full list of
 * affected names. */
const struct {
    const char *alias;
    const char *target;
} cli_feature_aliases[] = {
    {.alias = "integer_motion", .target = "motion"},
    {.alias = "integer_motion2", .target = "motion_v2"},
    {.alias = "integer_ssim", .target = "ssim"},
    {.alias = "integer_ms_ssim", .target = "float_ms_ssim"},
    {.alias = "integer_psnr", .target = "psnr"},
};

CLIFeatureConfig parse_feature_config(const char *const optarg, const char *const app)
{
    const size_t optarg_sz = strnlen(optarg, 1024);
    char *optarg_copy = static_cast<char *>(malloc(optarg_sz + 1));
    if (!optarg_copy)
        usage(app, "error while parsing feature option: %s", optarg);
    (void)memset(optarg_copy, 0, optarg_sz + 1);
    (void)strncpy(optarg_copy, optarg, optarg_sz);
    void *buf = optarg_copy;

    CLIFeatureConfig feature_cfg = {
        .name = vmaf_cli_split(&optarg_copy, '='),
        .opts_dict = nullptr,
        .buf = buf,
    };

    /* Rewrite user-facing "integer_*" aliases to the names the extractor
     * registry actually uses.  The rewrite only touches the name field; any
     * key=value options that follow the "=" separator are unaffected. */
    for (const auto &feature_alias : cli_feature_aliases) {
        if (!strcmp(feature_cfg.name, feature_alias.alias)) {
            feature_cfg.name = feature_alias.target;
            break;
        }
    }

    char *key_val = nullptr;
    while ((key_val = vmaf_cli_split(&optarg_copy, ':')) != nullptr) {
        const char *const key = vmaf_cli_split(&key_val, '=');
        const char *const val = vmaf_cli_split(&key_val, '=');
        if (!val) {
            usage(app,
                  "Problem parsing feature \"%s\", "
                  "bad option string \"%s\".\n",
                  feature_cfg.name, key);
        }
        const int err = vmaf_feature_dictionary_set(&feature_cfg.opts_dict, key, val);
        if (err)
            usage(app, "Problem parsing feature \"%s\"\n", optarg);
    }

    return feature_cfg;
}

void aom_ctc_v1_0(CLISettings *const settings, const char *const app)
{
    const CLIModelConfig cfg = {
        .path = nullptr,
        .version = "vmaf_v0.6.1", /* vmaf-model-pin: AOM CTC v1.0 mandates this exact model */
        .cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg, app, "models");

    const CLIModelConfig cfg_neg = {
        .path = nullptr,
        .version = "vmaf_v0.6.1neg", /* vmaf-model-pin: AOM CTC v1.0 mandates this exact model */
        .cfg = {.name = "vmaf_neg", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg_neg, app, "models");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr=reduced_hbd_peak=true:"
                                        "enable_apsnr=true:min_sse=0.5",
                                        app),
                   app, "features");

    checked_append(settings->feature_cfg, settings->feature_cnt, parse_feature_config("ciede", app),
                   app, "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ms_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr_hvs", app), app, "features");
}

void aom_ctc_v2_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v1_0(settings, app);
}

void aom_ctc_v3_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v2_0(settings, app);
    checked_append(settings->feature_cfg, settings->feature_cnt, parse_feature_config("cambi", app),
                   app, "features");
}

void aom_ctc_v4_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v3_0(settings, app);
}

void aom_ctc_v5_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v4_0(settings, app);
}

void aom_ctc_v6_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v5_0(settings, app);
    settings->common_bitdepth = true;
}

void aom_ctc_v7_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v6_0(settings, app);
    checked_replace_feature(
        settings->feature_cfg, settings->feature_cnt,
        parse_feature_config("float_ssim=scale=1:enable_db=true:clip_db=true", app), app,
        "features");
}

void parse_aom_ctc(CLISettings *const settings, const char *const optarg, const char *const app)
{
    using sv = std::string_view;
    const sv arg{optarg};

    if (arg == "proposed")
        usage(app, "`--aom_ctc proposed` is deprecated.");

    if (arg == "v1.0") {
        aom_ctc_v1_0(settings, app);
        return;
    }
    if (arg == "v2.0") {
        aom_ctc_v2_0(settings, app);
        return;
    }
    if (arg == "v3.0") {
        aom_ctc_v3_0(settings, app);
        return;
    }
    if (arg == "v4.0") {
        aom_ctc_v4_0(settings, app);
        return;
    }
    if (arg == "v5.0") {
        aom_ctc_v5_0(settings, app);
        return;
    }
    if (arg == "v6.0") {
        aom_ctc_v6_0(settings, app);
        return;
    }
    if (arg == "v7.0") {
        aom_ctc_v7_0(settings, app);
        return;
    }

    usage(app, "bad aom_ctc version \"%s\"", optarg);
}

void nflx_ctc_v1_0(CLISettings *const settings, const char *const app)
{
    const CLIModelConfig cfg = {
        .path = nullptr,
        .version = "vmaf_4k_v0.6.1",
        .cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg, app, "models");

    const CLIModelConfig cfg_neg = {
        .path = nullptr,
        .version = "vmaf_4k_v0.6.1neg",
        .cfg = {.name = "vmaf_neg", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg_neg, app, "models");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr=enable_chroma=true:enable_apsnr=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt, parse_feature_config("cambi", app),
                   app, "features");
}

void parse_nflx_ctc(CLISettings *const settings, const char *const optarg, const char *const app)
{
    if (std::string_view{optarg} == "v1.0") {
        nflx_ctc_v1_0(settings, app);
        return;
    }
    usage(app, "bad nflx_ctc version \"%s\"", optarg);
}

void handle_video_input_flag(const int o, const char *const optarg, const char *const app,
                             CLISettings *const settings)
{
    switch (o) {
    case 'r':
        settings->path_ref = const_cast<char *>(optarg);
        break;
    case 'd':
        settings->path_dist = const_cast<char *>(optarg);
        break;
    case 'w':
        settings->width = parse_unsigned(optarg, 'w', app);
        settings->use_yuv = true;
        break;
    case 'h':
        settings->height = parse_unsigned(optarg, 'h', app);
        settings->use_yuv = true;
        break;
    case 'p':
        settings->pix_fmt = parse_pix_fmt(optarg, 'p', app);
        settings->use_yuv = true;
        break;
    case 'b':
        settings->bitdepth = parse_bitdepth(optarg, 'b', app);
        settings->use_yuv = true;
        break;
    case 'o':
        settings->output_path = const_cast<char *>(optarg);
        break;
    default:
        break;
    }
}

void handle_threads_flag(const char *const optarg, const char *const app,
                         CLISettings *const settings)
{
    settings->thread_cnt = parse_unsigned(optarg, ARG_THREADS, app);
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const unsigned hw_threads = static_cast<unsigned>(si.dwNumberOfProcessors);
#else
    const long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    const unsigned hw_threads = (nproc > 0) ? static_cast<unsigned>(nproc) : 0u;
#endif
    if (hw_threads > 0u && settings->thread_cnt > hw_threads) {
        (void)std::fprintf(stderr, "warning: --threads %u capped to %u (hardware cores)\n",
                           settings->thread_cnt, hw_threads);
        settings->thread_cnt = hw_threads;
    }
}

void handle_frame_flag(const int o, const char *const optarg, const char *const app,
                       CLISettings *const settings)
{
    switch (o) {
    case ARG_FRAME_CNT:
        settings->frame_cnt = parse_unsigned(optarg, ARG_FRAME_CNT, app);
        break;
    case ARG_FRAME_SKIP_REF:
        settings->frame_skip_ref = parse_unsigned(optarg, ARG_FRAME_SKIP_REF, app);
        break;
    case ARG_FRAME_SKIP_DIST:
        settings->frame_skip_dist = parse_unsigned(optarg, ARG_FRAME_SKIP_DIST, app);
        break;
    default:
        break;
    }
}

void handle_tiny_flag(const int o, const char *const optarg, const char *const app,
                      CLISettings *const settings)
{
    switch (o) {
    case ARG_TINY_MODEL:
        settings->tiny_model_path = optarg;
        break;
    case ARG_TINY_DEVICE:
    case ARG_DNN_EP: {
        using sv = std::string_view;
        const sv dev{optarg};
        if (dev != "auto" && dev != "cpu" && dev != "cuda" && dev != "openvino" &&
            dev != "openvino-npu" && dev != "openvino-cpu" && dev != "openvino-gpu" &&
            dev != "coreml" && dev != "coreml-ane" && dev != "coreml-gpu" && dev != "coreml-cpu" &&
            dev != "rocm") {
            error(app, optarg, o == ARG_DNN_EP ? ARG_DNN_EP : ARG_TINY_DEVICE,
                  "one of auto|cpu|cuda|openvino|openvino-npu|openvino-cpu|"
                  "openvino-gpu|coreml|coreml-ane|coreml-gpu|coreml-cpu|rocm");
        }
        settings->tiny_device = optarg;
        break;
    }
    case ARG_TINY_THREADS:
        settings->tiny_threads = static_cast<int>(parse_unsigned(optarg, ARG_TINY_THREADS, app));
        break;
    case ARG_TINY_FP16:
        settings->tiny_fp16 = true;
        break;
    case ARG_TINY_MODEL_VERIFY:
        settings->tiny_model_verify = true;
        break;
    case ARG_TINY_CODEC:
        settings->tiny_codec = optarg;
        break;
    case ARG_TINY_PRESET:
        settings->tiny_preset = optarg;
        break;
    case ARG_TINY_CRF: {
        const unsigned long crf = parse_unsigned(optarg, ARG_TINY_CRF, app);
        settings->tiny_crf = static_cast<int>(crf > 63u ? 63u : crf);
        break;
    }
    case ARG_NO_REFERENCE:
        settings->no_reference = true;
        break;
    case ARG_TINY_RESIZE: {
        using sv = std::string_view;
        const sv rsz{optarg};
        if (rsz != "bilinear" && rsz != "nearest" && rsz != "bicubic" && rsz != "disabled") {
            error(app, optarg, ARG_TINY_RESIZE,
                  "--tiny-resize must be one of: bilinear, nearest, bicubic, disabled");
        }
        settings->tiny_resize = optarg;
        break;
    }
    default:
        break;
    }
}

void handle_backend_device_flag(const int o, const char *const optarg, const char *const app,
                                CLISettings *const settings)
{
    switch (o) {
    case ARG_NO_CUDA:
        settings->no_cuda = true;
        break;
    case ARG_NO_SYCL:
        settings->no_sycl = true;
        break;
    case ARG_SYCL_DEVICE:
        settings->sycl_device = static_cast<int>(parse_unsigned(optarg, ARG_SYCL_DEVICE, app));
        break;
    case ARG_NO_HIP:
        settings->no_hip = true;
        break;
    case ARG_HIP_DEVICE:
        settings->hip_device = static_cast<int>(parse_unsigned(optarg, ARG_HIP_DEVICE, app));
        break;
    case ARG_NO_METAL:
        settings->no_metal = true;
        break;
    case ARG_METAL_DEVICE:
        settings->metal_device = static_cast<int>(parse_unsigned(optarg, ARG_METAL_DEVICE, app));
        break;
    case ARG_BACKEND:
        settings->backend = optarg;
        break;
    default:
        break;
    }
}

void apply_backend_settings(const char *const app, CLISettings *const settings)
{
    if (settings->backend) {
        using sv = std::string_view;
        const sv be{settings->backend};
        if (be == "auto") {
            if (!settings->no_cuda && !settings->use_gpumask) {
                settings->gpumask = 0;
                settings->use_gpumask = true;
            }
        } else if (be == "cpu") {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_hip = true;
            settings->no_metal = true;
        } else if (be == "cuda") {
            settings->no_sycl = true;
            settings->no_hip = true;
            settings->no_metal = true;
            if (!settings->use_gpumask) {
                settings->gpumask = 0;
                settings->use_gpumask = true;
            }
        } else if (be == "sycl") {
            settings->no_cuda = true;
            settings->no_hip = true;
            settings->no_metal = true;
            if (settings->sycl_device < 0)
                settings->sycl_device = 0;
        } else if (be == "hip") {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_metal = true;
            if (settings->hip_device < 0)
                settings->hip_device = 0;
        } else if (be == "metal") {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_hip = true;
            if (settings->metal_device < 0)
                settings->metal_device = 0;
        } else {
            usage(app,
                  "Unknown --backend value '%s' "
                  "(expected: auto|cpu|cuda|sycl|hip|metal)",
                  settings->backend);
        }
    } else {
        if (!settings->no_cuda && !settings->use_gpumask) {
            settings->gpumask = 0;
            settings->use_gpumask = true;
        }
    }
}

void validate_cli_settings(const char *const app, CLISettings *const settings)
{
    if (settings->no_reference) {
        if (!settings->tiny_model_path) {
            usage(app, "--no-reference requires --tiny-model; no classic NR scorer exists");
        }
        settings->no_prediction = true;
    } else if (!settings->path_ref) {
        usage(app, "Reference .y4m or .yuv (-r/--reference) is required");
    }
    if (!settings->path_dist)
        usage(app, "Distorted .y4m or .yuv (-d/--distorted) is required");

    if (settings->use_yuv && settings->width == 0 &&
        (settings->height || settings->pix_fmt || settings->bitdepth)) {
        usage(app, "--width must be > 0");
    }
    if (settings->use_yuv && settings->height == 0 &&
        (settings->width || settings->pix_fmt || settings->bitdepth)) {
        usage(app, "--height must be > 0");
    }
    if (settings->use_yuv &&
        !(settings->width && settings->height && settings->pix_fmt && settings->bitdepth)) {
        usage(app, "The following options are required for .yuv input:\n"
                   "  --width/-w\n"
                   "  --height/-h\n"
                   "  --pixel_format/-p\n"
                   "  --bitdepth/-b\n");
    }

    if (settings->model_cnt == 0 && !settings->no_prediction) {
#if VMAF_BUILT_IN_MODELS
        const CLIModelConfig cfg = {
            .path = nullptr,
            .version =
                settings->netflix_compat ?
                    VMAF_NETFLIX_COMPAT_MODEL_VERSION :
                    VMAF_DEFAULT_MODEL_VERSION, /* vmaf-model-pin: Netflix upstream compat restores v0.6.1 default model */
            .cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT},
            .feature_overload = {},
            .overload_cnt = 0,
            .buf = nullptr,
            .is_default = true,
        };
        checked_append(settings->model_config, settings->model_cnt, cfg, app, "models");
#else
        usage(app, "At least one model (-m/--model) is required "
                   "unless no prediction (-n/--no_prediction) is set");
#endif
    }

    for (unsigned i = 0; i < settings->model_cnt; i++) {
        for (unsigned j = 0; j < settings->model_cnt; j++) {
            if (i == j)
                continue;
            if (!strcmp(settings->model_config[i].cfg.name, settings->model_config[j].cfg.name)) {
                usage(app, "Each model should be uniquely named. "
                           "Set using `--model` via the `name=...` param.");
            }
        }
    }
}

void handle_output_flag(const int o, CLISettings *const settings)
{
    switch (o) {
    case ARG_OUTPUT_XML:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_XML;
        break;
    case ARG_OUTPUT_JSON:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_JSON;
        break;
    case ARG_OUTPUT_CSV:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_CSV;
        break;
    case ARG_OUTPUT_SUB:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_SUB;
        break;
    default:
        break;
    }
}

void handle_feature_model_flag(const int o, const char *const optarg, const char *const app,
                               CLISettings *const settings)
{
    switch (o) {
    case 'm':
        checked_append(settings->model_config, settings->model_cnt, parse_model_config(optarg, app),
                       app, "models");
        break;
    case ARG_FEATURE:
        checked_append(settings->feature_cfg, settings->feature_cnt,
                       parse_feature_config(optarg, app), app, "features");
        break;
    case ARG_THREADS:
        handle_threads_flag(optarg, app, settings);
        break;
    case ARG_SUBSAMPLE:
        settings->subsample = parse_unsigned(optarg, ARG_SUBSAMPLE, app);
        break;
    case 'c':
    case ARG_CPUMASK:
        settings->cpumask = parse_unsigned(optarg, ARG_CPUMASK, app);
        break;
    case ARG_GPUMASK:
        settings->gpumask = parse_unsigned(optarg, ARG_GPUMASK, app);
        settings->use_gpumask = true;
        break;
    case ARG_AOM_CTC:
        parse_aom_ctc(settings, optarg, app);
        break;
    case ARG_NFLX_CTC:
        parse_nflx_ctc(settings, optarg, app);
        break;
    default:
        break;
    }
}

void handle_misc_flag(const int o, const char *const app, CLISettings *const settings)
{
    switch (o) {
    case ARG_HELP:
        usage(app, nullptr);
        break;
    case 'n':
        settings->no_prediction = true;
        break;
    case 'q':
        settings->quiet = true;
        break;
    case ARG_NETFLIX_COMPAT:
        settings->netflix_compat = true;
        break;
    case 'v':
        if (settings->vmafx_mode) {
            (void)fprintf(stderr, "VMAFX %s (auto-backend, precision=max)\n", vmaf_version());
        } else {
            (void)fprintf(stderr, "%s\n", vmaf_version());
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: CLI version exit
        exit(0);
    default:
        break;
    }
}

bool handle_primary_cli_opt(const int o, const char *const optarg, const char *const app,
                            CLISettings *const settings)
{
    switch (o) {
    case 'r':
    case 'd':
    case 'w':
    case 'h':
    case 'p':
    case 'b':
    case 'o':
        handle_video_input_flag(o, optarg, app, settings);
        return true;
    case ARG_OUTPUT_XML:
    case ARG_OUTPUT_JSON:
    case ARG_OUTPUT_CSV:
    case ARG_OUTPUT_SUB:
        handle_output_flag(o, settings);
        return true;
    case 'm':
    case ARG_FEATURE:
    case ARG_THREADS:
    case ARG_SUBSAMPLE:
    case 'c':
    case ARG_CPUMASK:
    case ARG_GPUMASK:
    case ARG_AOM_CTC:
    case ARG_NFLX_CTC:
        handle_feature_model_flag(o, optarg, app, settings);
        return true;
    default:
        return false;
    }
}

void process_single_cli_opt(const int o, const char *const optarg, const char *const app,
                            CLISettings *const settings)
{
    if (handle_primary_cli_opt(o, optarg, app, settings))
        return;

    switch (o) {
    case ARG_FRAME_CNT:
    case ARG_FRAME_SKIP_REF:
    case ARG_FRAME_SKIP_DIST:
        handle_frame_flag(o, optarg, app, settings);
        break;
    case ARG_NO_CUDA:
    case ARG_NO_SYCL:
    case ARG_SYCL_DEVICE:
    case ARG_NO_HIP:
    case ARG_HIP_DEVICE:
    case ARG_NO_METAL:
    case ARG_METAL_DEVICE:
    case ARG_BACKEND:
        handle_backend_device_flag(o, optarg, app, settings);
        break;
    case ARG_PRECISION:
        settings->precision_fmt = resolve_precision_fmt(optarg, app, settings);
        break;
    case ARG_TINY_MODEL:
    case ARG_TINY_DEVICE:
    case ARG_DNN_EP:
    case ARG_TINY_THREADS:
    case ARG_TINY_FP16:
    case ARG_TINY_MODEL_VERIFY:
    case ARG_TINY_CODEC:
    case ARG_TINY_PRESET:
    case ARG_TINY_CRF:
    case ARG_NO_REFERENCE:
    case ARG_TINY_RESIZE:
        handle_tiny_flag(o, optarg, app, settings);
        break;
    case ARG_HELP:
    case 'n':
    case 'q':
    case 'v':
    case ARG_NETFLIX_COMPAT:
        handle_misc_flag(o, app, settings);
        break;
    default:
        break;
    }
}

} // namespace

extern "C" bool detect_vmafx_mode(const char *const argv0)
{
    if (!argv0)
        return false;
    using sv = std::string_view;
    const sv s{argv0};
    const auto slash = s.find_last_of("/\\");
    const sv base = (slash != sv::npos) ? s.substr(slash + 1) : s;
    return (base == "vmafx" || base == "vmafx.exe");
}

void cli_parse(const int argc, char *const *const argv, CLISettings *const settings)
{
    (void)memset(settings, 0, sizeof(*settings));
    settings->vmafx_mode = detect_vmafx_mode(argv[0]);
    settings->sycl_device = -1;  // auto-select by default
    settings->hip_device = -1;   // auto-select by default
    settings->metal_device = -1; // auto-select by default
    settings->precision_n = -1;
    settings->precision_fmt = VMAF_DEFAULT_PRECISION_FMT;
    settings->tiny_device = "auto";
    settings->tiny_crf = -1; /* ADR-0522: -1 = unset; 0..63 user-supplied */
    int o = 0;

    // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: single-threaded CLI entry point before worker threads spawn
    while ((o = getopt_long(argc, argv, short_opts, long_opts, nullptr)) >= 0) {
        process_single_cli_opt(o, optarg, argv[0], settings);
    }

    if (settings->vmafx_mode) {
        /* ADR-0690: apply modernized defaults (precision=max) unless explicit --precision given */
        if (!settings->precision_max && !settings->precision_legacy &&
            (settings->precision_n == -1)) {
            settings->precision_max = true;
            settings->precision_fmt = VMAF_LOSSLESS_PRECISION_FMT;
        }
    }

    if (settings->netflix_compat) {
        /* ADR-0696: Final post-parse pass overriding any modernizations back to legacy defaults */
        settings->backend = "cpu";
        settings->no_cuda = true;
        settings->no_sycl = true;
        settings->no_hip = true;
        settings->no_metal = true;
        settings->precision_max = false;
        settings->precision_legacy = true;
        settings->precision_n = -1;
        settings->precision_fmt = VMAF_DEFAULT_PRECISION_FMT;
    }

    if (!settings->output_fmt)
        settings->output_fmt = VMAF_OUTPUT_FORMAT_XML;

    apply_backend_settings(argv[0], settings);
    validate_cli_settings(argv[0], settings);
}

void cli_free(CLISettings *const settings)
{
    for (unsigned i = 0; i < settings->model_cnt; i++)
        free(settings->model_config[i].buf);
    for (unsigned i = 0; i < settings->feature_cnt; i++)
        free(settings->feature_cfg[i].buf);
}
