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

#ifdef _WIN32
#include "compat/win32/getopt.h"
#include <windows.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cli_parse.h"

#include "config.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

static const char short_opts[] = "r:d:w:h:p:b:m:c:o:nvq";

enum {
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
};

/* Default matches Netflix's pre-fork output exactly so the CPU golden
 * gate passes without explicit flags (CLAUDE.md §8). Round-trip lossless
 * formatting is opt-in via --precision=max. See ADR-0119 (supersedes
 * ADR-0006). */
#define VMAF_DEFAULT_PRECISION_FMT "%.6f"
#define VMAF_LOSSLESS_PRECISION_FMT "%.17g"

static char precision_fmt_buf[16];

static const char *resolve_precision_fmt(const char *optarg, const char *app, CLISettings *s)
{
    if (!strcmp(optarg, "max") || !strcmp(optarg, "full")) {
        s->precision_max = true;
        return VMAF_LOSSLESS_PRECISION_FMT;
    }
    if (!strcmp(optarg, "legacy")) {
        /* `legacy` is now the default; keep the alias accepted so existing
         * scripts that pass it explicitly do not break. */
        s->precision_legacy = true;
        return VMAF_DEFAULT_PRECISION_FMT;
    }
    char *end;
    long n = strtol(optarg, &end, 10);
    if (*end || end == optarg || n < 1 || n > 17) {
        (void)fprintf(stderr,
                      "%s: --precision must be an integer 1..17, "
                      "or one of: max, full, legacy (got: %s)\n",
                      app, optarg);
        exit(1);
    }
    s->precision_n = (int)n;
    (void)snprintf(precision_fmt_buf, sizeof precision_fmt_buf, "%%.%ldg", n);
    return precision_fmt_buf;
}

static const struct option long_opts[] = {
    {"reference", 1, NULL, 'r'},
    {"distorted", 1, NULL, 'd'},
    {"width", 1, NULL, 'w'},
    {"height", 1, NULL, 'h'},
    {"pixel_format", 1, NULL, 'p'},
    {"bitdepth", 1, NULL, 'b'},
    {"model", 1, NULL, 'm'},
    {"output", 1, NULL, 'o'},
    {"xml", 0, NULL, ARG_OUTPUT_XML},
    {"json", 0, NULL, ARG_OUTPUT_JSON},
    {"csv", 0, NULL, ARG_OUTPUT_CSV},
    {"sub", 0, NULL, ARG_OUTPUT_SUB},
    {"help", 0, NULL, ARG_HELP},
    {"threads", 1, NULL, ARG_THREADS},
    {"feature", 1, NULL, ARG_FEATURE},
    {"subsample", 1, NULL, ARG_SUBSAMPLE},
    {"cpumask", 1, NULL, ARG_CPUMASK},
    {"gpumask", 1, NULL, ARG_GPUMASK},
    {"aom_ctc", 1, NULL, ARG_AOM_CTC},
    {"nflx_ctc", 1, NULL, ARG_NFLX_CTC},
    {"frame_cnt", 1, NULL, ARG_FRAME_CNT},
    {"frame_skip_ref", 1, NULL, ARG_FRAME_SKIP_REF},
    {"frame_skip_dist", 1, NULL, ARG_FRAME_SKIP_DIST},
    {"no_cuda", 0, NULL, ARG_NO_CUDA},
    {"no_sycl", 0, NULL, ARG_NO_SYCL},
    {"sycl_device", 1, NULL, ARG_SYCL_DEVICE},
    {"no_hip", 0, NULL, ARG_NO_HIP},
    {"hip_device", 1, NULL, ARG_HIP_DEVICE},
    {"no_metal", 0, NULL, ARG_NO_METAL},
    {"metal_device", 1, NULL, ARG_METAL_DEVICE},
    {"backend", 1, NULL, ARG_BACKEND},
    {"precision", 1, NULL, ARG_PRECISION},
    {"tiny-model", 1, NULL, ARG_TINY_MODEL},
    {"tiny_model", 1, NULL, ARG_TINY_MODEL},
    {"tiny-device", 1, NULL, ARG_TINY_DEVICE},
    {"tiny_device", 1, NULL, ARG_TINY_DEVICE},
    {"tiny-threads", 1, NULL, ARG_TINY_THREADS},
    {"tiny_threads", 1, NULL, ARG_TINY_THREADS},
    {"tiny-fp16", 0, NULL, ARG_TINY_FP16},
    {"tiny_fp16", 0, NULL, ARG_TINY_FP16},
    {"tiny-model-verify", 0, NULL, ARG_TINY_MODEL_VERIFY},
    {"tiny_model_verify", 0, NULL, ARG_TINY_MODEL_VERIFY},
    /* ADR-0519 — codec context. Underscore aliases match the rest
     * of the tiny-* family for scripting consistency. */
    {"tiny-codec", 1, NULL, ARG_TINY_CODEC},
    {"tiny_codec", 1, NULL, ARG_TINY_CODEC},
    {"tiny-preset", 1, NULL, ARG_TINY_PRESET},
    {"tiny_preset", 1, NULL, ARG_TINY_PRESET},
    {"tiny-crf", 1, NULL, ARG_TINY_CRF},
    {"tiny_crf", 1, NULL, ARG_TINY_CRF},
    {"no-reference", 0, NULL, ARG_NO_REFERENCE},
    {"no_reference", 0, NULL, ARG_NO_REFERENCE},
    /* ADR-0550 — NCHW tiny-model auto-resize filter selector. */
    {"tiny-resize", 1, NULL, ARG_TINY_RESIZE},
    {"tiny_resize", 1, NULL, ARG_TINY_RESIZE},
    /* --dnn-ep is the user-facing name for selecting the ONNX Runtime
     * execution provider. It is an alias for --tiny-device so both flags
     * write to the same CLISettings.tiny_device field. Accepting both names
     * lets users follow the ORT "execution provider" terminology directly
     * without knowing the fork's internal "tiny-device" naming. */
    {"dnn-ep", 1, NULL, ARG_DNN_EP},
    {"dnn_ep", 1, NULL, ARG_DNN_EP},
    {"no_prediction", 0, NULL, 'n'},
    {"version", 0, NULL, 'v'},
    {"quiet", 0, NULL, 'q'},
    {NULL, 0, NULL, 0},
};

_Noreturn static void usage(const char *const app, const char *const reason, ...);
static void usage(const char *const app, const char *const reason, ...)
{
    /* When invoked via --help (reason == NULL) print to stdout and exit 0.
     * When invoked as an error handler (reason != NULL) print to stderr and
     * exit 1 so scripts can detect incorrect usage. */
    const int exit_code = reason ? 1 : 0;
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        va_list args;
        va_start(args, reason);
        (void)vfprintf(out, reason, args);
        va_end(args);
        (void)fprintf(out, "\n\n");
    }
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
    /* C99 only requires compilers to support string literals up to 4095 chars
     * (5.2.4.1). Split the usage text in two fprintf calls so we stay under
     * the limit even as new flags accrete. */
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
        " --version/-v:                print version and exit\n");
    exit(exit_code);
}

#define CHECKED_APPEND(arr, cnt, val, app, desc)                                                   \
    do {                                                                                           \
        if ((cnt) == CLI_SETTINGS_STATIC_ARRAY_LEN)                                                \
            usage((app), "A maximum of %d %s are supported\n", CLI_SETTINGS_STATIC_ARRAY_LEN,      \
                  (desc));                                                                         \
        (arr)[(cnt)++] = (val);                                                                    \
    } while (0)

#define CHECKED_REPLACE(arr, cnt, val, app, desc)                                                  \
    do {                                                                                           \
        CLIFeatureConfig _val = (val);                                                             \
        unsigned _i;                                                                               \
        for (_i = 0; _i < (cnt); _i++)                                                             \
            if (!strcmp((arr)[_i].name, _val.name)) {                                              \
                free((arr)[_i].buf);                                                               \
                vmaf_feature_dictionary_free(&(arr)[_i].opts_dict);                                \
                (arr)[_i] = _val;                                                                  \
                break;                                                                             \
            }                                                                                      \
        if (_i == (cnt))                                                                           \
            CHECKED_APPEND((arr), (cnt), _val, (app), (desc));                                     \
    } while (0)

static void error(const char *const app, const char *const optarg, const int option,
                  const char *const shouldbe)
{
    char optname[256];
    int n;

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
        return; /* unreachable — usage() calls exit(1); satisfies _Noreturn analysis */
    }
    if (long_opts[n].val < 256) {
        (void)snprintf(optname, sizeof(optname), "-%c/--%s", long_opts[n].val, long_opts[n].name);
    } else {
        (void)snprintf(optname, sizeof(optname), "--%s", long_opts[n].name);
    }

    usage(app, "Invalid argument \"%s\" for option %s; should be %s", optarg, optname, shouldbe);
}

static unsigned parse_unsigned(const char *const optarg, const int option, const char *const app)
{
    /* Reject negative strings before calling strtoul: POSIX strtoul silently
     * converts "-1" to ULONG_MAX via unsigned wrapping, which would then be
     * truncated to UINT_MAX and silently accepted. */
    if (optarg[0] == '-')
        error(app, optarg, option, "a non-negative integer");
    char *end;
    errno = 0;
    const unsigned long ul = strtoul(optarg, &end, 0);
    if (*end || end == optarg || errno == ERANGE || ul > UINT_MAX)
        error(app, optarg, option, "an integer in [0, 2^32-1]");
    return (unsigned)ul;
}

static unsigned parse_bitdepth(const char *const optarg, const int option, const char *const app)
{
    unsigned bitdepth = parse_unsigned(optarg, option, app);
    if (!((bitdepth == 8) || (bitdepth == 10) || (bitdepth == 12) || (bitdepth == 16)))
        error(app, optarg, option, "a valid bitdepth (8/10/12/16)");
    return bitdepth;
}

static enum VmafPixelFormat parse_pix_fmt(const char *const optarg, const int option,
                                          const char *const app)
{
    enum VmafPixelFormat pix_fmt = VMAF_PIX_FMT_UNKNOWN;

    if (!strcmp(optarg, "420"))
        pix_fmt = VMAF_PIX_FMT_YUV420P;
    if (!strcmp(optarg, "422"))
        pix_fmt = VMAF_PIX_FMT_YUV422P;
    if (!strcmp(optarg, "444"))
        pix_fmt = VMAF_PIX_FMT_YUV444P;

    if (!pix_fmt) {
        error(app, optarg, option,
              "a valid pixel format "
              "(420/422/444)");
    }

    return pix_fmt;
}

#ifndef HAVE_STRSEP
static char *strsep(char **sp, char *sep)
{
    char *p, *s;
    if (sp == NULL || *sp == NULL || **sp == '\0')
        return NULL;
    s = *sp;
    p = s + strcspn(s, sep);
    if (*p != '\0')
        *p++ = '\0';
    *sp = p;
    return s;
}
#endif

static CLIModelConfig parse_model_config(const char *const optarg, const char *const app)
{
    const size_t optarg_sz = strnlen(optarg, 1024);
    char *optarg_copy = malloc(optarg_sz + 1);
    if (!optarg_copy)
        usage(app, "error while parsing model option: %s", optarg);
    memset(optarg_copy, 0, optarg_sz + 1);
    strncpy(optarg_copy, optarg, optarg_sz);

    CLIModelConfig model_cfg = {
        .cfg =
            {
                .name = "vmaf",
                .flags = VMAF_MODEL_FLAGS_DEFAULT,
            },
        .path = NULL,
        .version = NULL,
        .buf = optarg_copy,
    };

    char *key_val;
    while ((key_val = strsep(&optarg_copy, ":")) != NULL) {
        char *key = strsep(&key_val, "=");
        char *val = strsep(&key_val, "=");
        if (!val) {
            if (!strcmp(key, "disable_clip")) {
                val = "true";
            } else if (!strcmp(key, "enable_transform")) {
                val = "true";
            } else {
                usage(app,
                      "Problem parsing model, "
                      "bad option string \"%s\".",
                      key);
            }
        }

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
            char *name = strsep(&key, ".");
            model_cfg.feature_overload[model_cfg.overload_cnt].name = name;
            char *opt = strsep(&key, ".");
            int err = vmaf_feature_dictionary_set(
                &model_cfg.feature_overload[model_cfg.overload_cnt].opts_dict, opt, val);
            if (err)
                usage(app, "Problem parsing model: \"%s\"\n", name);

            model_cfg.overload_cnt++;
        }
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
static const struct {
    const char *alias;
    const char *target;
} cli_feature_aliases[] = {
    {"integer_motion", "motion"}, {"integer_motion2", "motion_v2"},
    {"integer_ssim", "ssim"},     {"integer_ms_ssim", "float_ms_ssim"},
    {"integer_psnr", "psnr"},
};

static CLIFeatureConfig parse_feature_config(const char *const optarg, const char *const app)
{
    const size_t optarg_sz = strnlen(optarg, 1024);
    char *optarg_copy = malloc(optarg_sz + 1);
    if (!optarg_copy)
        usage(app, "error while parsing feature option: %s", optarg);
    memset(optarg_copy, 0, optarg_sz + 1);
    strncpy(optarg_copy, optarg, optarg_sz);
    void *buf = optarg_copy;

    CLIFeatureConfig feature_cfg = {
        .name = strsep(&optarg_copy, "="),
        .opts_dict = NULL,
        .buf = buf,
    };

    /* Rewrite user-facing "integer_*" aliases to the names the extractor
     * registry actually uses.  The rewrite only touches the name field; any
     * key=value options that follow the "=" separator are unaffected. */
    for (unsigned ai = 0; ai < sizeof(cli_feature_aliases) / sizeof(cli_feature_aliases[0]); ai++) {
        if (!strcmp(feature_cfg.name, cli_feature_aliases[ai].alias)) {
            feature_cfg.name = cli_feature_aliases[ai].target;
            break;
        }
    }

    char *key_val;
    while ((key_val = strsep(&optarg_copy, ":")) != NULL) {
        const char *key = strsep(&key_val, "=");
        const char *val = strsep(&key_val, "=");
        if (!val) {
            usage(app,
                  "Problem parsing feature \"%s\","
                  " bad option string \"%s\".\n",
                  feature_cfg.name, key);
        }
        int err = vmaf_feature_dictionary_set(&feature_cfg.opts_dict, key, val);
        if (err)
            usage(app, "Problem parsing feature \"%s\"\n", optarg);
    }

    return feature_cfg;
}

static void aom_ctc_v1_0(CLISettings *settings, const char *const app)
{
    CLIModelConfig cfg = {
        .version = "vmaf_v0.6.1",
        .cfg = {.name = "vmaf"},
    };
    CHECKED_APPEND(settings->model_config, settings->model_cnt, cfg, app, "models");

    CLIModelConfig cfg_neg = {
        .version = "vmaf_v0.6.1neg",
        .cfg = {.name = "vmaf_neg"},
    };
    CHECKED_APPEND(settings->model_config, settings->model_cnt, cfg_neg, app, "models");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr=reduced_hbd_peak=true:"
                                        "enable_apsnr=true:min_sse=0.5",
                                        app),
                   app, "features");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt, parse_feature_config("ciede", app),
                   app, "features");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ms_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr_hvs", app), app, "features");
}

static void aom_ctc_v2_0(CLISettings *settings, const char *app)
{
    aom_ctc_v1_0(settings, app);
}

static void aom_ctc_v3_0(CLISettings *settings, const char *app)
{
    aom_ctc_v2_0(settings, app);
    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt, parse_feature_config("cambi", app),
                   app, "features");
}

static void aom_ctc_v4_0(CLISettings *settings, const char *app)
{
    aom_ctc_v3_0(settings, app);
}

static void aom_ctc_v5_0(CLISettings *settings, const char *app)
{
    aom_ctc_v4_0(settings, app);
}

static void aom_ctc_v6_0(CLISettings *settings, const char *app)
{
    aom_ctc_v5_0(settings, app);
    settings->common_bitdepth = true;
}

static void aom_ctc_v7_0(CLISettings *settings, const char *app)
{
    aom_ctc_v6_0(settings, app);
    CHECKED_REPLACE(settings->feature_cfg, settings->feature_cnt,
                    parse_feature_config("float_ssim=scale=1:enable_db=true:clip_db=true", app),
                    app, "features");
}

static void parse_aom_ctc(CLISettings *settings, const char *const optarg, const char *const app)
{
    if (!strcmp(optarg, "proposed"))
        usage(app, "`--aom_ctc proposed` is deprecated.");

    if (!strcmp(optarg, "v1.0")) {
        aom_ctc_v1_0(settings, app);
        return;
    }

    if (!strcmp(optarg, "v2.0")) {
        aom_ctc_v2_0(settings, app);
        return;
    }

    if (!strcmp(optarg, "v3.0")) {
        aom_ctc_v3_0(settings, app);
        return;
    }

    if (!strcmp(optarg, "v4.0")) {
        aom_ctc_v4_0(settings, app);
        return;
    }

    if (!strcmp(optarg, "v5.0")) {
        aom_ctc_v5_0(settings, app);
        return;
    }

    if (!strcmp(optarg, "v6.0")) {
        aom_ctc_v6_0(settings, app);
        return;
    }

    if (!strcmp(optarg, "v7.0")) {
        aom_ctc_v7_0(settings, app);
        return;
    }

    usage(app, "bad aom_ctc version \"%s\"", optarg);
}

static void nflx_ctc_v1_0(CLISettings *settings, const char *const app)
{
    CLIModelConfig cfg = {
        .version = "vmaf_4k_v0.6.1",
        .cfg = {.name = "vmaf"},
    };
    CHECKED_APPEND(settings->model_config, settings->model_cnt, cfg, app, "models");

    CLIModelConfig cfg_neg = {
        .version = "vmaf_4k_v0.6.1neg",
        .cfg = {.name = "vmaf_neg"},
    };
    CHECKED_APPEND(settings->model_config, settings->model_cnt, cfg_neg, app, "models");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr=enable_chroma=true:enable_apsnr=true", app), app,
                   "features");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt, parse_feature_config("cambi", app),
                   app, "features");
}

static void parse_nflx_ctc(CLISettings *settings, const char *const optarg, const char *const app)
{
    if (!strcmp(optarg, "v1.0")) {
        nflx_ctc_v1_0(settings, app);
        return;
    }

    usage(app, "bad nflx_ctc version \"%s\"", optarg);
}

void cli_parse(const int argc, char *const *const argv, CLISettings *const settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->sycl_device = -1;  // auto-select by default
    settings->hip_device = -1;   // auto-select by default
    settings->metal_device = -1; // auto-select by default
    settings->precision_n = -1;
    settings->precision_fmt = VMAF_DEFAULT_PRECISION_FMT;
    settings->tiny_device = "auto";
    settings->tiny_crf = -1; /* ADR-0522: -1 = unset; 0..63 user-supplied */
    int o;

    while ((o = getopt_long(argc, argv, short_opts, long_opts, NULL)) >= 0) {
        switch (o) {
        case 'r':
            settings->path_ref = optarg;
            break;
        case 'd':
            settings->path_dist = optarg;
            break;
        case 'w':
            settings->width = parse_unsigned(optarg, 'w', argv[0]);
            settings->use_yuv = true;
            break;
        case 'h':
            settings->height = parse_unsigned(optarg, 'h', argv[0]);
            settings->use_yuv = true;
            break;
        case 'p':
            settings->pix_fmt = parse_pix_fmt(optarg, 'p', argv[0]);
            settings->use_yuv = true;
            break;
        case 'b':
            settings->bitdepth = parse_bitdepth(optarg, 'b', argv[0]);
            settings->use_yuv = true;
            break;
        case 'o':
            settings->output_path = optarg;
            break;
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
        case 'm':
            CHECKED_APPEND(settings->model_config, settings->model_cnt,
                           parse_model_config(optarg, argv[0]), argv[0], "models");
            break;
        case ARG_FEATURE:
            CHECKED_APPEND(settings->feature_cfg, settings->feature_cnt,
                           parse_feature_config(optarg, argv[0]), argv[0], "features");
            break;
        /* The three handlers below pass the long-only enum value (not a
         * synthesised short-option char) to parse_unsigned() so that
         * error()'s walk over long_opts[] finds a matching entry. The
         * earlier 't' / 's' / 'c' shape tripped error()'s
         * assert(long_opts[n].name) for any non-numeric optarg
         * (e.g. `--threads abc`), turning a clean usage() error into a
         * SIGABRT — surfaced by the libFuzzer harness in PR #408
         * (ADR-0311). See ADR-0316.
         *
         * INVARIANT (ADR-0438): every short option declared in short_opts[]
         * must have a case arm in this switch.  The 'c' arm was absent until
         * the audit that produced ADR-0438 — getopt_long consumed -c <val>
         * from the command line but the switch fell into default: and silently
         * discarded the argument.  The fall-through below mirrors the
         * ARG_TINY_DEVICE / ARG_DNN_EP alias pattern already in this switch. */
        case ARG_THREADS:
            settings->thread_cnt = parse_unsigned(optarg, ARG_THREADS, argv[0]);
            {
                /* Cap thread count to the number of online hardware cores to
                 * prevent OOM from absurdly large --threads values (e.g.
                 * --threads 10000 on a 16-core host).  sysconf returns -1 on
                 * error; in that case we leave the user-supplied value intact
                 * and trust the thread pool to handle it gracefully. */
#ifdef _WIN32
                SYSTEM_INFO si;
                GetSystemInfo(&si);
                unsigned hw_threads = (unsigned)si.dwNumberOfProcessors;
#else
                long nproc = sysconf(_SC_NPROCESSORS_ONLN);
                unsigned hw_threads = (nproc > 0) ? (unsigned)nproc : 0u;
#endif
                if (hw_threads > 0u && settings->thread_cnt > hw_threads) {
                    (void)fprintf(stderr, "warning: --threads %u capped to %u (hardware cores)\n",
                                  settings->thread_cnt, hw_threads);
                    settings->thread_cnt = hw_threads;
                }
            }
            break;
        case ARG_SUBSAMPLE:
            settings->subsample = parse_unsigned(optarg, ARG_SUBSAMPLE, argv[0]);
            break;
        case 'c':
        /* fall through — -c is the short form of --cpumask; both write
         * settings->cpumask via ARG_CPUMASK so error() reports the long
         * name on bad input. */
        case ARG_CPUMASK:
            settings->cpumask = parse_unsigned(optarg, ARG_CPUMASK, argv[0]);
            break;
        case ARG_GPUMASK:
            settings->gpumask = parse_unsigned(optarg, ARG_GPUMASK, argv[0]);
            settings->use_gpumask = true;
            break;
        case ARG_AOM_CTC:
            parse_aom_ctc(settings, optarg, argv[0]);
            break;
        case ARG_NFLX_CTC:
            parse_nflx_ctc(settings, optarg, argv[0]);
            break;
        case ARG_FRAME_CNT:
            settings->frame_cnt = parse_unsigned(optarg, ARG_FRAME_CNT, argv[0]);
            break;
        case ARG_FRAME_SKIP_REF:
            settings->frame_skip_ref = parse_unsigned(optarg, ARG_FRAME_SKIP_REF, argv[0]);
            break;
        case ARG_FRAME_SKIP_DIST:
            settings->frame_skip_dist = parse_unsigned(optarg, ARG_FRAME_SKIP_DIST, argv[0]);
            break;
        case ARG_NO_CUDA:
            settings->no_cuda = true;
            break;
        case ARG_NO_SYCL:
            settings->no_sycl = true;
            break;
        case ARG_SYCL_DEVICE:
            settings->sycl_device = (int)parse_unsigned(optarg, ARG_SYCL_DEVICE, argv[0]);
            break;
        case ARG_NO_HIP:
            settings->no_hip = true;
            break;
        case ARG_HIP_DEVICE:
            settings->hip_device = (int)parse_unsigned(optarg, ARG_HIP_DEVICE, argv[0]);
            break;
        case ARG_NO_METAL:
            settings->no_metal = true;
            break;
        case ARG_METAL_DEVICE:
            settings->metal_device = (int)parse_unsigned(optarg, ARG_METAL_DEVICE, argv[0]);
            break;
        case ARG_BACKEND:
            settings->backend = optarg;
            break;
        case ARG_PRECISION:
            settings->precision_fmt = resolve_precision_fmt(optarg, argv[0], settings);
            break;
        case ARG_TINY_MODEL:
            settings->tiny_model_path = optarg;
            break;
        case ARG_TINY_DEVICE:
        /* fall through — --dnn-ep is an alias; both write tiny_device */
        case ARG_DNN_EP:
            if (strcmp(optarg, "auto") && strcmp(optarg, "cpu") && strcmp(optarg, "cuda") &&
                strcmp(optarg, "openvino") && strcmp(optarg, "openvino-npu") &&
                strcmp(optarg, "openvino-cpu") && strcmp(optarg, "openvino-gpu") &&
                strcmp(optarg, "coreml") && strcmp(optarg, "coreml-ane") &&
                strcmp(optarg, "coreml-gpu") && strcmp(optarg, "coreml-cpu") &&
                strcmp(optarg, "rocm")) {
                error(argv[0], optarg, o == ARG_DNN_EP ? ARG_DNN_EP : ARG_TINY_DEVICE,
                      "one of auto|cpu|cuda|openvino|openvino-npu|openvino-cpu|"
                      "openvino-gpu|coreml|coreml-ane|coreml-gpu|coreml-cpu|rocm");
            }
            settings->tiny_device = optarg;
            break;
        case ARG_TINY_THREADS:
            settings->tiny_threads = (int)parse_unsigned(optarg, ARG_TINY_THREADS, argv[0]);
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
            /* parse_unsigned exits on overflow / negative; we then
             * clamp at use-time to [0, 63] per ADR-0519. Keep the
             * accepted range generous so a stray CRF=51 from x265
             * is fine without an explicit upper cap here. */
            unsigned long crf = parse_unsigned(optarg, ARG_TINY_CRF, argv[0]);
            settings->tiny_crf = (int)(crf > 63u ? 63u : crf);
            break;
        }
        case ARG_NO_REFERENCE:
            settings->no_reference = true;
            break;
        case ARG_TINY_RESIZE:
            /* ADR-0543 — validate the keyword up front so a typo
             * surfaces at parse time instead of after model load. */
            if (strcmp(optarg, "bilinear") != 0 && strcmp(optarg, "nearest") != 0 &&
                strcmp(optarg, "bicubic") != 0 && strcmp(optarg, "disabled") != 0) {
                error(argv[0], optarg, ARG_TINY_RESIZE,
                      "--tiny-resize must be one of: bilinear, nearest, bicubic, disabled");
            }
            settings->tiny_resize = optarg;
            break;
        case 'n':
            settings->no_prediction = true;
            break;
        case 'q':
            settings->quiet = true;
            break;
        case ARG_HELP:
            usage(argv[0], NULL);
            break; /* unreachable — usage() is _Noreturn */
        case 'v':
            (void)fprintf(stderr, "%s\n", vmaf_version());
            exit(0);
        default:
            break;
        }
    }

    if (!settings->output_fmt)
        settings->output_fmt = VMAF_OUTPUT_FORMAT_XML;
    /* --backend exclusive selector. Apply BEFORE the rest of the
     * post-parse validation so the per-backend flags are consistent
     * downstream. Must run before any code path that consumes
     * settings->no_cuda / no_sycl. */
    if (settings->backend) {
        if (!strcmp(settings->backend, "auto")) {
            /* Default — leave per-backend flags as-is, BUT engage CUDA
             * dispatch when compiled in and not explicitly disabled.
             * Without this, `vmaf` (or any caller passing `--backend auto`,
             * which is the default) silently runs on CPU even when the
             * libvmaf build has CUDA + the host has an RTX 4090: the
             * dispatch only routes to the CUDA extractors when
             * `use_gpumask = true` (see the `--backend cuda` branch below
             * for the why-comment). Mirroring that line for `auto` makes
             * auto-select actually pick a GPU when one is available, while
             * still respecting `--no_cuda` and gracefully degrading on
             * a non-CUDA libvmaf build (init_gpu_backends no-ops). */
            if (!settings->no_cuda && !settings->use_gpumask) {
                settings->gpumask = 0;
                settings->use_gpumask = true;
            }
        } else if (!strcmp(settings->backend, "cpu")) {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_hip = true;
            settings->no_metal = true;
        } else if (!strcmp(settings->backend, "cuda")) {
            settings->no_sycl = true;
            settings->no_hip = true;
            settings->no_metal = true;
            if (!settings->use_gpumask) {
                /* `gpumask` is a CUDA-*disable* bitmask per the public
                 * VmafConfiguration::gpumask contract — `compute_fex_flags`
                 * routes the CUDA dispatch slot only when `gpumask == 0`.
                 * Setting `use_gpumask = true` triggers `vmaf_cuda_state_init`
                 * in the CLI; leaving `gpumask = 0` lets the runtime then
                 * actually pick the CUDA extractors. Earlier revisions set
                 * `gpumask = 1` here intending it as a device-pin, which
                 * silently disabled CUDA and routed everything through the
                 * CPU path — see test_backend_cuda_engages_cuda below. */
                settings->gpumask = 0;
                settings->use_gpumask = true;
            }
        } else if (!strcmp(settings->backend, "sycl")) {
            settings->no_cuda = true;
            settings->no_hip = true;
            settings->no_metal = true;
            if (settings->sycl_device < 0)
                settings->sycl_device = 0;
        } else if (!strcmp(settings->backend, "hip")) {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_metal = true;
            if (settings->hip_device < 0)
                settings->hip_device = 0;
        } else if (!strcmp(settings->backend, "metal")) {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_hip = true;
            if (settings->metal_device < 0)
                settings->metal_device = 0;
        } else {
            usage(argv[0],
                  "Unknown --backend value '%s' "
                  "(expected: auto|cpu|cuda|sycl|hip|metal)",
                  settings->backend);
        }
    } else {
        /* No `--backend` passed at all: behaves like `--backend auto`.
         * Engage CUDA dispatch on the same terms as the explicit
         * `--backend auto` branch above — without this, the binary
         * silently runs on CPU even when the libvmaf build has CUDA
         * and the host has an NVIDIA GPU. */
        if (!settings->no_cuda && !settings->use_gpumask) {
            settings->gpumask = 0;
            settings->use_gpumask = true;
        }
    }

    /* ADR-0520: `--no-reference` opts out of the reference-required gate.
     * The only scorer that can produce a value without a reference is a
     * no-reference tiny ONNX model, so a tiny-model path is mandatory in
     * NR mode. The classic SVM models all consume FR feature columns
     * (vif/adm/motion), so the built-in default is also suppressed below
     * and `--no_prediction` is forced on. */
    if (settings->no_reference) {
        if (!settings->tiny_model_path) {
            usage(argv[0], "--no-reference requires --tiny-model; no classic NR scorer exists");
        }
        /* Suppress the built-in vmaf_v0.6.1 default; the SVM is FR-only. */
        settings->no_prediction = true;
    } else if (!settings->path_ref) {
        usage(argv[0], "Reference .y4m or .yuv (-r/--reference) is required");
    }
    if (!settings->path_dist)
        usage(argv[0], "Distorted .y4m or .yuv (-d/--distorted) is required");
    if (settings->use_yuv &&
        !(settings->width && settings->height && settings->pix_fmt && settings->bitdepth)) {
        usage(argv[0], "The following options are required for .yuv input:\n"
                       "  --width/-w\n"
                       "  --height/-h\n"
                       "  --pixel_format/-p\n"
                       "  --bitdepth/-b\n");
    }

    if (settings->model_cnt == 0 && !settings->no_prediction) {
#if VMAF_BUILT_IN_MODELS
        CLIModelConfig cfg = {
            .version = "vmaf_v0.6.1",
        };
        CHECKED_APPEND(settings->model_config, settings->model_cnt, cfg, argv[0], "models");
#else
        usage(argv[0], "At least one model (-m/--model) is required "
                       "unless no prediction (-n/--no_prediction) is set");
#endif
    }

    for (unsigned i = 0; i < settings->model_cnt; i++) {
        for (unsigned j = 0; j < settings->model_cnt; j++) {
            if (i == j)
                continue;
            if (!strcmp(settings->model_config[i].cfg.name, settings->model_config[j].cfg.name)) {
                usage(argv[0], "Each model should be uniquely named. "
                               "Set using `--model` via the `name=...` param.");
            }
        }
    }
}

void cli_free(CLISettings *settings)
{
    for (unsigned i = 0; i < settings->model_cnt; i++)
        free(settings->model_config[i].buf);
    for (unsigned i = 0; i < settings->feature_cnt; i++)
        free(settings->feature_cfg[i].buf);
}
