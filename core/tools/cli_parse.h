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

#ifndef VMAF_CLI_PARSE_H
#define VMAF_CLI_PARSE_H

/* ADR-0809: extern "C" guards so cli_parse.cpp and vmaf.cpp (C++23 TUs)
 * can include this header without name-mangling the C-linkage functions. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/feature.h"

#define CLI_SETTINGS_STATIC_ARRAY_LEN 32

/* NOLINTBEGIN(modernize-use-using): one definition for C and C++. The CLI
 * (cli_parse.cpp, vmaf.cpp) is C++ and the parser's tests and fuzz harness
 * (test_cli_parse.c, test_cli_parse_long_only_args.c, fuzz_cli_parse.c) are C;
 * `using` is not C, and a `using` copy next to a `typedef` would be two
 * definitions to keep equal by hand. ADR-0141. */
typedef struct {
    const char *name;
    VmafFeatureDictionary *opts_dict;
    void *buf;
} CLIFeatureConfig;

typedef struct {
    const char *path;
    const char *version;
    VmafModelConfig cfg;
    struct {
        const char *name;
        VmafFeatureDictionary *opts_dict;
    } feature_overload[CLI_SETTINGS_STATIC_ARRAY_LEN];
    unsigned overload_cnt;
    void *buf;
    bool is_default;
} CLIModelConfig;

/* Members are ordered by alignment (pointers, the two tables, 4-byte values,
 * flags), so the structure has no interior padding. Nothing initialises it by
 * position: the CLI zero-initialises it and cli_parse() assigns by name. */
typedef struct {
    /* --- Pointers --- */
    char *path_ref;
    char *path_dist;
    char *output_path;
    /* --backend exclusive selector: "auto" (default, all enabled
     * backends compete by registry order), "cpu", "cuda", "sycl",
     * "hip", "metal". Setting one disables the others via the
     * existing --no_X flags before they're consumed. */
    const char *backend;
    const char *precision_fmt; // resolved printf format, e.g. "%.6f"
    /* Phase 3k — tiny-AI surface (all unset by default). */
    const char *tiny_model_path; /* NULL = no tiny model */
    const char *tiny_device;     /* "auto"|"cpu"|"cuda"|"openvino"|
                                  * "coreml"|"coreml-ane"|"coreml-gpu"|
                                  * "coreml-cpu"|"openvino-npu"|
                                  * "openvino-cpu"|"openvino-gpu"|"rocm" */
    /* ADR-0519 — codec context for codec-aware tiny models
     * (e.g. fr_regressor_v2). tiny_codec, tiny_preset and tiny_crf all
     * default unset; when any is provided the CLI calls
     * vmaf_dnn_set_codec_context after the model attaches. tiny_crf is -1
     * when unset (passed through as "use 0/63 = 0.0" to the model only if
     * the user explicitly set a codec). */
    const char *tiny_codec;
    const char *tiny_preset;
    /* ADR-0550 — NCHW tiny-model auto-resize filter. NULL = unset
     * (libvmaf default of bilinear applies). Accepted values:
     * "bilinear" (default), "nearest", "bicubic", "disabled". */
    const char *tiny_resize;
    /* RC4 WP5 (#2142): the JSON report `--verify-provenance` re-runs and
     * compares; NULL = a scoring run. */
    const char *verify_provenance;

    /* --- Models and features --- */
    CLIModelConfig model_config[CLI_SETTINGS_STATIC_ARRAY_LEN];
    CLIFeatureConfig feature_cfg[CLI_SETTINGS_STATIC_ARRAY_LEN];

    /* --- 4-byte values --- */
    unsigned frame_skip_ref;
    unsigned frame_skip_dist;
    unsigned frame_cnt;
    unsigned width;
    unsigned height;
    enum VmafPixelFormat pix_fmt;
    unsigned bitdepth;
    VmafColor color_ref, color_dist; /* ed61076b2; all four attributes or none */
    enum VmafOutputFormat output_fmt;
    unsigned model_cnt;
    unsigned feature_cnt;
    enum VmafLogLevel log_level;
    unsigned subsample;
    unsigned thread_cnt;
    unsigned cpumask;
    unsigned gpumask;
    int sycl_device;  // -1 = not requested (default), 0+ = device index
    int hip_device;   // -1 = not requested (default), 0+ = device index
    int metal_device; // -1 = not requested (default), 0+ = device index
    int precision_n;  // -1 = unset (default %.6f), else user N
    int tiny_threads; /* 0 = ORT default */
    int tiny_crf;     /* ADR-0519, see tiny_codec */

    /* --- Flags --- */
    bool use_yuv;
    bool no_prediction;
    bool quiet;
    bool common_bitdepth;
    bool use_gpumask; // true only when --gpumask was explicitly passed
    bool no_cuda;
    bool no_sycl;
    bool no_hip;
    bool no_metal;
    bool precision_max;    // --precision=max|full given (selects %.17g)
    bool precision_legacy; // --precision=legacy given (alias for the default)
    bool tiny_fp16;
    bool no_reference;       /* skip reference; only meaningful with NR tiny model */
    bool provenance_sidecar; /* RC4 WP5: also write <output>.provenance.json */
    /* T6-9 / ADR-0211 — Sigstore-bundle verification of tiny models. When
     * true, the CLI calls vmaf_dnn_verify_signature() before model load and
     * exits non-zero on any verification failure (missing registry entry,
     * missing bundle, cosign exec error, cosign exit non-zero). Off by
     * default for dev-friendliness; production deployments set it on. */
    bool tiny_model_verify;
    /* ADR-0690: true when binary invoked as vmafx (argv[0] basename detection). */
    bool vmafx_mode;
    /* ADR-0696: true when --netflix-compat passed to restore legacy defaults. */
    bool netflix_compat;
    /* ADR-1874: --list-backends; print the backend report instead of scoring. */
    bool list_backends;
    /* ADR-1918: --check-sample-range; vmaf_set_sample_range_check_enabled(). */
    bool check_sample_range;
} CLISettings;
/* NOLINTEND(modernize-use-using) */

bool detect_vmafx_mode(const char *argv0);

void cli_parse(const int argc, char *const *const argv, CLISettings *const settings);

/* Release the option buffers and every option dictionary the settings still
 * own. A caller that passes an `opts_dict` to a libvmaf call that takes it
 * (vmaf_use_feature(), vmaf_model_feature_overload(),
 * vmaf_model_collection_feature_overload()) clears the pointer here first. */
void cli_free(CLISettings *settings);

/* RC4 WP5: the report of `--verify-provenance <report>` (or
 * `--verify-provenance=<report>`) in argv, or NULL for a scoring run. */
const char *cli_verify_provenance_arg(int argc, char *const *argv);

/* Let the next cli_parse() scan a new argv from its start (getopt keeps its
 * position between scans). */
void cli_parse_reset(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_CLI_PARSE_H */
