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
#else
#include <getopt.h>
#endif

#include "test.h"

#include "cli_parse.h"
#include <string.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

static int cli_free_dicts(CLISettings *settings)
{
    for (unsigned i = 0; i < settings->feature_cnt; i++) {
        int err = vmaf_feature_dictionary_free(&(settings->feature_cfg[i].opts_dict));
        if (err)
            return err;
    }
    return 0;
}

static char *test_aom_ctc_v1_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--aom_ctc", "v1.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --aom_ctc v1.0 provided but common_bitdepth enabled",
              !settings.common_bitdepth);
    mu_assert("cli_parse: --aom_ctc v1.0 provided but number of features is not 5",
              settings.feature_cnt == 5);
    mu_assert("cli_parse: --aom_ctc v1.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

static char *test_aom_ctc_v2_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--aom_ctc", "v2.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --aom_ctc v2.0 provided but common_bitdepth enabled",
              !settings.common_bitdepth);
    mu_assert("cli_parse: --aom_ctc v2.0 provided but number of features is not 5",
              settings.feature_cnt == 5);
    mu_assert("cli_parse: --aom_ctc v2.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

static char *test_aom_ctc_v3_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--aom_ctc", "v3.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --aom_ctc v3.0 provided but common_bitdepth enabled",
              !settings.common_bitdepth);
    mu_assert("cli_parse: --aom_ctc v3.0 provided but number of features is not 6",
              settings.feature_cnt == 6);
    mu_assert("cli_parse: --aom_ctc v3.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

static char *test_aom_ctc_v4_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--aom_ctc", "v4.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --aom_ctc v4.0 provided but common_bitdepth enabled",
              !settings.common_bitdepth);
    mu_assert("cli_parse: --aom_ctc v4.0 provided but number of features is not 6",
              settings.feature_cnt == 6);
    mu_assert("cli_parse: --aom_ctc v4.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

static char *test_aom_ctc_v5_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--aom_ctc", "v5.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --aom_ctc v5.0 provided but common_bitdepth enabled",
              !settings.common_bitdepth);
    mu_assert("cli_parse: --aom_ctc v5.0 provided but number of features is not 6",
              settings.feature_cnt == 6);
    mu_assert("cli_parse: --aom_ctc v5.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

static char *test_aom_ctc_v6_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--aom_ctc", "v6.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --aom_ctc v6.0 provided but common_bitdepth not enabled",
              settings.common_bitdepth);
    mu_assert("cli_parse: --aom_ctc v6.0 provided but number of features is not 6",
              settings.feature_cnt == 6);
    mu_assert("cli_parse: --aom_ctc v6.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

static char *test_nflx_ctc_v1_0()
{
    char *argv[7] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--nflx_ctc", "v1.0"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --nflx_ctc v1.0 provided but common_bitdepth enabled",
              !settings.common_bitdepth);
    mu_assert("cli_parse: --nflx_ctc v1.0 provided but number of features is not 3",
              settings.feature_cnt == 3);
    mu_assert("cli_parse: --nflx_ctc v1.0 provided but number of models is not 2",
              settings.model_cnt == 2);
    cli_free(&settings);
    cli_free_dicts(&settings);

    return NULL;
}

/* `--backend cuda` must end up with gpumask == 0 (NOT 1), because
 * VmafConfiguration::gpumask is a CUDA-*disable* bitmask — any nonzero
 * value disables CUDA in compute_fex_flags. The CLI's job is only to
 * trip use_gpumask so vmaf_cuda_state_init runs; the runtime then
 * picks the CUDA extractors because gpumask is 0. Earlier revisions
 * set gpumask = 1 here, which silently routed every "CUDA" run
 * through the CPU path. */
static char *test_backend_cuda_engages_cuda()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--backend", "cuda"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --backend cuda must set use_gpumask = true (so CUDA inits)",
              settings.use_gpumask);
    mu_assert("cli_parse: --backend cuda must set gpumask = 0 (any nonzero DISABLES CUDA)",
              settings.gpumask == 0);
    mu_assert("cli_parse: --backend cuda must set no_sycl = true", settings.no_sycl);
    mu_assert("cli_parse: --backend cuda must set no_hip = true", settings.no_hip);
    mu_assert("cli_parse: --backend cuda must set no_metal = true", settings.no_metal);
    mu_assert("cli_parse: --backend cuda must NOT set no_cuda", !settings.no_cuda);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *test_backend_cpu()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--backend", "cpu"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --backend cpu must set no_cuda = true", settings.no_cuda);
    mu_assert("cli_parse: --backend cpu must set no_sycl = true", settings.no_sycl);
    mu_assert("cli_parse: --backend cpu must set no_hip = true", settings.no_hip);
    mu_assert("cli_parse: --backend cpu must set no_metal = true", settings.no_metal);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *test_backend_sycl()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--backend", "sycl"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --backend sycl must set no_cuda = true", settings.no_cuda);
    mu_assert("cli_parse: --backend sycl must set no_hip = true", settings.no_hip);
    mu_assert("cli_parse: --backend sycl must set no_metal = true", settings.no_metal);
    mu_assert("cli_parse: --backend sycl must default sycl_device to 0", settings.sycl_device == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

/* test_backend_vulkan removed — ADR-0726: Vulkan backend dropped. */

static char *test_backend_hip()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--backend", "hip"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --backend hip must set no_cuda = true", settings.no_cuda);
    mu_assert("cli_parse: --backend hip must set no_sycl = true", settings.no_sycl);
    mu_assert("cli_parse: --backend hip must set no_metal = true", settings.no_metal);
    mu_assert("cli_parse: --backend hip must NOT set no_hip", !settings.no_hip);
    mu_assert("cli_parse: --backend hip must default hip_device to 0", settings.hip_device == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *test_backend_metal()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--backend", "metal"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --backend metal must set no_cuda = true", settings.no_cuda);
    mu_assert("cli_parse: --backend metal must set no_sycl = true", settings.no_sycl);
    mu_assert("cli_parse: --backend metal must set no_hip = true", settings.no_hip);
    mu_assert("cli_parse: --backend metal must NOT set no_metal", !settings.no_metal);
    mu_assert("cli_parse: --backend metal must default metal_device to 0",
              settings.metal_device == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *test_hip_device_explicit()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--hip_device", "2"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --hip_device 2 must set hip_device = 2", settings.hip_device == 2);
    mu_assert("cli_parse: --hip_device must not engage no_hip", !settings.no_hip);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *test_metal_device_explicit()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--metal_device", "1"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --metal_device 1 must set metal_device = 1", settings.metal_device == 1);
    mu_assert("cli_parse: --metal_device must not engage no_metal", !settings.no_metal);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *test_no_hip_no_metal_flags()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--no_hip", "--no_metal"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --no_hip must set no_hip = true", settings.no_hip);
    mu_assert("cli_parse: --no_metal must set no_metal = true", settings.no_metal);
    mu_assert("cli_parse: --no_hip must leave hip_device at default -1", settings.hip_device == -1);
    mu_assert("cli_parse: --no_metal must leave metal_device at default -1",
              settings.metal_device == -1);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

/* Regression for audit finding F1 / ADR-0438: '-c' is declared in
 * short_opts[] but the switch previously had no 'case 'c'' arm, so
 * getopt_long consumed the option value and the switch fell into
 * default:, silently discarding the cpumask.  The fix adds a
 * 'case 'c':' fall-through before ARG_CPUMASK. */
static char *test_cpumask_short_opt()
{
    /* -c 0xff must set settings.cpumask = 255, same as --cpumask 0xff. */
    char *argv[9] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "-c", "0xff"};
    int argc = 7;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: -c 0xff must set cpumask = 255 (was silently dropped before ADR-0438)",
              settings.cpumask == 255);
    cli_free(&settings);
    cli_free_dicts(&settings);

    /* Decimal value: -c 3 */
    char *argv2[9] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "-c", "3"};
    int argc2 = 7;
    CLISettings settings2;
    optind = 1;
    cli_parse(argc2, argv2, &settings2);
    mu_assert("cli_parse: -c 3 must set cpumask = 3", settings2.cpumask == 3);
    cli_free(&settings2);
    cli_free_dicts(&settings2);

    return NULL;
}

/* Explicit `--gpumask=N --backend cuda` must preserve the user's gpumask,
 * NOT clobber it. Multi-GPU rigs need fine-grained disable bits. */
static char *test_backend_cuda_preserves_explicit_gpumask()
{
    char *argv[8] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--gpumask=2", "--backend", "cuda"};
    int argc = 8;
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("cli_parse: --gpumask=2 --backend cuda must preserve gpumask = 2",
              settings.gpumask == 2);
    mu_assert("cli_parse: --gpumask=2 --backend cuda must keep use_gpumask = true",
              settings.use_gpumask);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *run_aom_ctc_tests(void)
{
    mu_run_test(test_aom_ctc_v1_0);
    mu_run_test(test_aom_ctc_v2_0);
    mu_run_test(test_aom_ctc_v3_0);
    mu_run_test(test_aom_ctc_v4_0);
    mu_run_test(test_aom_ctc_v5_0);
    mu_run_test(test_aom_ctc_v6_0);
    mu_run_test(test_nflx_ctc_v1_0);
    return NULL;
}

static char *run_backend_tests(void)
{
    mu_run_test(test_backend_cpu);
    mu_run_test(test_backend_cuda_engages_cuda);
    mu_run_test(test_backend_cuda_preserves_explicit_gpumask);
    mu_run_test(test_backend_sycl);
    /* test_backend_vulkan removed — ADR-0726 */
    mu_run_test(test_backend_hip);
    mu_run_test(test_backend_metal);
    mu_run_test(test_hip_device_explicit);
    mu_run_test(test_metal_device_explicit);
    mu_run_test(test_no_hip_no_metal_flags);
    mu_run_test(test_cpumask_short_opt);
    return NULL;
}

/* ADR-0520: `--no-reference --tiny-model X --distorted Y` must reach
 * the success path without a reference path. The parser sets
 * `no_reference`, defers the reference-required gate, and force-enables
 * `no_prediction` so the built-in SVM default (which would auto-load
 * with `model_cnt == 0`) is not injected — the SVM consumes FR feature
 * columns and would always fail downstream. The failure case (NR mode
 * without `--tiny-model`) calls the `_Noreturn` `usage()` and so cannot
 * be exercised in-process; the shell smoke at `libvmaf/test/dnn/test_cli.sh`
 * §5a covers it. */
static char *test_no_reference_with_tiny_model_passes_parse(void)
{
    char *argv[] = {"vmaf",         "--no-reference",
                    "--tiny-model", "/tmp/m.onnx",
                    "-d",           "/tmp/d.yuv",
                    "-w",           "64",
                    "-h",           "64",
                    "-p",           "420",
                    "-b",           "8"};
    const int argc = sizeof(argv) / sizeof(argv[0]);
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("ADR-0520: --no-reference must be recorded", settings.no_reference);
    mu_assert("ADR-0520: --no-reference must force no_prediction so the "
              "built-in vmaf_v0.6.1 SVM is not auto-injected",
              settings.no_prediction);
    mu_assert("ADR-0520: --no-reference must suppress the default-model "
              "auto-add (model_cnt remains 0 with no -m flag)",
              settings.model_cnt == 0);
    mu_assert("ADR-0519: --tiny-model path must be captured", settings.tiny_model_path != NULL);
    mu_assert("ADR-0520: --no-reference must allow path_ref to be NULL", settings.path_ref == NULL);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

/* Underscore alias must take the same code path. */
static char *test_no_reference_underscore_alias_parses(void)
{
    char *argv[] = {"vmaf",         "--no_reference",
                    "--tiny_model", "/tmp/m.onnx",
                    "-d",           "/tmp/d.yuv",
                    "-w",           "64",
                    "-h",           "64",
                    "-p",           "420",
                    "-b",           "8"};
    const int argc = sizeof(argv) / sizeof(argv[0]);
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("ADR-0520: --no_reference (underscore alias) must be recorded",
              settings.no_reference);
    mu_assert("ADR-0519: underscore alias must also force no_prediction", settings.no_prediction);
    cli_free(&settings);
    cli_free_dicts(&settings);
    return NULL;
}

static char *run_no_reference_tests(void)
{
    mu_run_test(test_no_reference_with_tiny_model_passes_parse);
    mu_run_test(test_no_reference_underscore_alias_parses);
    return NULL;
}

/* ADR-0690: detect_vmafx_mode correctly parses argv[0] basename */
static char *test_detect_vmafx_mode(void)
{
    mu_assert("detect_vmafx_mode: 'vmafx' must return true", detect_vmafx_mode("vmafx"));
    mu_assert("detect_vmafx_mode: '/usr/local/bin/vmafx' must return true",
              detect_vmafx_mode("/usr/local/bin/vmafx"));
    mu_assert("detect_vmafx_mode: './build/tools/vmafx.exe' must return true",
              detect_vmafx_mode("./build/tools/vmafx.exe"));
    mu_assert("detect_vmafx_mode: 'vmaf' must return false", !detect_vmafx_mode("vmaf"));
    mu_assert("detect_vmafx_mode: '/usr/local/bin/vmaf' must return false",
              detect_vmafx_mode("/usr/local/bin/vmaf") == false);
    mu_assert("detect_vmafx_mode: 'vmaf_bench' must return false",
              detect_vmafx_mode("vmaf_bench") == false);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    mu_assert("detect_vmafx_mode: NULL must return false", detect_vmafx_mode(NULL) == false);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

/* ADR-0690: vmafx invocation defaults to precision=max and modern default model */
static char *test_vmafx_mode_defaults(void)
{
    char *argv[] = {"vmafx", "-r", "ref.y4m", "-d", "dis.y4m"};
    const int argc = sizeof(argv) / sizeof(argv[0]);
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("ADR-0690: vmafx must set vmafx_mode = true", settings.vmafx_mode);
    mu_assert("ADR-0690: vmafx must default to precision_max = true", settings.precision_max);
    mu_assert("ADR-0690: vmafx must format with %.17g",
              strcmp(settings.precision_fmt, "%.17g") == 0);
    mu_assert("ADR-0690: vmafx must default to 1 model", settings.model_cnt == 1);
    mu_assert("ADR-0690: vmafx must default to VMAF_DEFAULT_MODEL_VERSION",
              strcmp(settings.model_config[0].version, VMAF_DEFAULT_MODEL_VERSION) == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

/* ADR-0696: --netflix-compat restores legacy CPU, %.6f, and v0.6.1 model */
/* --netflix-compat (ADR-0696): parse once, assert in two focused tests so each stays
 * under the readability-function-size thresholds. */
static void parse_netflix_compat(CLISettings *settings)
{
    char *argv[] = {"vmafx", "-r", "ref.y4m", "-d", "dis.y4m", "--netflix-compat"};
    const int argc = sizeof(argv) / sizeof(argv[0]);
    optind = 1;
    cli_parse(argc, argv, settings);
}

static char *test_netflix_compat_flag_precision(void)
{
    CLISettings settings;
    parse_netflix_compat(&settings);
    mu_assert("ADR-0696: netflix_compat flag must be set", settings.netflix_compat);
    mu_assert("ADR-0696: netflix_compat must force precision_max = false", !settings.precision_max);
    mu_assert("ADR-0696: netflix_compat must use %.6f precision",
              strcmp(settings.precision_fmt, "%.6f") == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

static char *test_netflix_compat_flag_backend_and_model(void)
{
    CLISettings settings;
    parse_netflix_compat(&settings);
    mu_assert("ADR-0696: netflix_compat must disable CUDA", settings.no_cuda);
    mu_assert("ADR-0696: netflix_compat must disable SYCL", settings.no_sycl);
    mu_assert("ADR-0696: netflix_compat must disable HIP", settings.no_hip);
    mu_assert("ADR-0696: netflix_compat must disable Metal", settings.no_metal);
    mu_assert("ADR-0696: netflix_compat must default to VMAF_NETFLIX_COMPAT_MODEL_VERSION (v0.6.1)",
              strcmp(settings.model_config[0].version, VMAF_NETFLIX_COMPAT_MODEL_VERSION) == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

/* ADR-0696: underscore alias --netflix_compat and override behavior */
static char *test_netflix_compat_underscore_override(void)
{
    char *argv[] = {"vmaf",      "-r",   "ref.y4m",     "-d",  "dis.y4m",
                    "--backend", "cuda", "--precision", "max", "--netflix_compat"};
    const int argc = sizeof(argv) / sizeof(argv[0]);
    CLISettings settings;
    optind = 1;
    cli_parse(argc, argv, &settings);
    mu_assert("ADR-0696: netflix_compat must be set", settings.netflix_compat);
    mu_assert("ADR-0696: netflix_compat must override --precision=max to %.6f",
              !settings.precision_max && strcmp(settings.precision_fmt, "%.6f") == 0);
    mu_assert("ADR-0696: netflix_compat must override --backend cuda to CPU",
              settings.no_cuda && settings.no_sycl && settings.no_hip && settings.no_metal);
    mu_assert("ADR-0696: netflix_compat model must be VMAF_NETFLIX_COMPAT_MODEL_VERSION",
              strcmp(settings.model_config[0].version, VMAF_NETFLIX_COMPAT_MODEL_VERSION) == 0);
    cli_free(&settings);
    cli_free_dicts(&settings);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

static char *run_vmafx_tests(void)
{
    mu_run_test(test_detect_vmafx_mode);
    mu_run_test(test_vmafx_mode_defaults);
    mu_run_test(test_netflix_compat_flag_precision);
    mu_run_test(test_netflix_compat_flag_backend_and_model);
    mu_run_test(test_netflix_compat_underscore_override);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

char *run_tests()
{
    char *result = run_aom_ctc_tests();
    if (result)
        return result;
    result = run_backend_tests();
    if (result)
        return result;
    result = run_no_reference_tests();
    if (result)
        return result;
    result = run_vmafx_tests();
    if (result)
        return result;
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
