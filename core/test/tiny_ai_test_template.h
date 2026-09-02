/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Tiny-AI extractor registration-test template.
 *
 *  Every tiny-AI extractor (`feature_lpips.c`, `fastdvdnet_pre.c`,
 *  `feature_mobilesal.c`, `transnet_v2.c`, …) ships ~120 LOC of
 *  near-identical structural smoke tests:
 *
 *    - `<name>_is_registered`        — extractor discoverable + non-NULL hooks
 *    - `<name>_provides_<feat>`      — provided_features[0] round-trips
 *    - `<name>_options_table`        — options well-formed + has model_path
 *    - `<name>_init_rejects_missing` — init() declines without model_path
 *
 *  This header emits all four via a single macro
 *  `VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS`, so each per-extractor test
 *  file shrinks from ~140 LOC to ~30 LOC. The 2026-05-02 whole-codebase
 *  dedup audit ranked this as the highest-leverage non-GPU dedup
 *  (480 LOC across 4 files, low risk).
 *
 *  Per-extractor test files keep ownership of their own `run_tests()` so
 *  they can append extractor-specific tests (e.g. the
 *  TransNet V2 provided_features list-termination check or the
 *  dual-feature-name surface) on top of the four standard ones the
 *  macro emits.
 *
 *  Power-of-10 / SEI CERT compliance: pure preprocessor token-paste; no
 *  control flow, no recursion, no allocation. The emitted test bodies
 *  contain only static-bounded loops over small option/feature tables.
 */

#ifndef LIBVMAF_TEST_TINY_AI_TEST_TEMPLATE_H_
#define LIBVMAF_TEST_TINY_AI_TEST_TEMPLATE_H_

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "config.h"
#include "libvmaf/dnn.h"
#include "feature/feature_extractor.h"
#include "opt.h"

/* MinGW's mingw.org / MSYS2 headers do not expose POSIX setenv/unsetenv
 * under -std=c11 -pedantic. Shim via _putenv_s (MSVCRT) on _WIN32; use
 * the POSIX functions elsewhere. Both shims silently swallow failures —
 * this is a test helper, not a production code path. */
#if defined(_WIN32)
static int vmaf_tiny_ai_test_setenv(const char *name, const char *value)
{
    return _putenv_s(name, value);
}
static int vmaf_tiny_ai_test_unsetenv(const char *name)
{
    return _putenv_s(name, "");
}
#else
static int vmaf_tiny_ai_test_setenv(const char *name, const char *value)
{
    return setenv(name, value, 1);
}
static int vmaf_tiny_ai_test_unsetenv(const char *name)
{
    return unsetenv(name);
}
#endif

/**
 * Emit the four standard registration tests for a tiny-AI extractor.
 *
 * @param ext_name      String literal: the extractor name as registered
 *                      (e.g. "lpips", "mobilesal").
 * @param feat_name     String literal: the primary feature name in
 *                      `provided_features[0]` (e.g. "lpips",
 *                      "saliency_mean", "shot_boundary_probability").
 * @param env_var       String literal: the model-path env var
 *                      (e.g. "VMAF_LPIPS_MODEL_PATH").
 * @param fn_prefix     Identifier prefix for the emitted functions
 *                      (e.g. `lpips`, `mobilesal`). Must be a valid C
 *                      identifier; used in the generated function names
 *                      so multiple template invocations in a single TU
 *                      stay distinct.
 *
 * Emits four `static char *` test functions named
 * `test_<fn_prefix>_is_registered`,
 * `test_<fn_prefix>_provides_primary_feature`,
 * `test_<fn_prefix>_options_table_well_formed`, and
 * `test_<fn_prefix>_init_rejects_missing_model`. Caller wires them
 * into its own `run_tests()` via `mu_run_test()`.
 */
#define VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS(ext_name, feat_name, env_var, fn_prefix)              \
    static mu_message_t test_##fn_prefix##_is_registered(void)                                       \
    {                                                                                                \
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(ext_name);                    \
        mu_assert(ext_name " extractor must be registered by name", fex != NULL);                    \
        mu_assert("registered extractor has wrong name", !strcmp(fex->name, ext_name));              \
        mu_assert(ext_name ".init must be set", fex->init != NULL);                                  \
        mu_assert(ext_name ".extract must be set", fex->extract != NULL);                            \
        mu_assert(ext_name ".close must be set", fex->close != NULL);                                \
        mu_assert(ext_name ".priv_size must be non-zero", fex->priv_size > 0u);                      \
        return NULL;                                                                                 \
    }                                                                                                \
                                                                                                     \
    static mu_message_t test_##fn_prefix##_provides_primary_feature(void)                            \
    {                                                                                                \
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_feature_name(feat_name, 0);        \
        mu_assert("'" feat_name "' feature name must resolve to an extractor", fex != NULL);         \
        mu_assert("'" feat_name "' must map to the " ext_name " extractor",                          \
                  !strcmp(fex->name, ext_name));                                                     \
        return NULL;                                                                                 \
    }                                                                                                \
                                                                                                     \
    static mu_message_t test_##fn_prefix##_options_table_well_formed(void)                           \
    {                                                                                                \
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(ext_name);                    \
        mu_assert(ext_name " extractor missing", fex != NULL);                                       \
        mu_assert(ext_name " must expose at least one option", fex->options != NULL);                \
                                                                                                     \
        int saw_model_path = 0;                                                                      \
        for (const VmafOption *opt = fex->options; opt && opt->name; ++opt) {                        \
            mu_assert("every option must have non-NULL help text", opt->help != NULL);               \
            if (!strcmp(opt->name, "model_path")) {                                                  \
                saw_model_path = 1;                                                                  \
                mu_assert("model_path must be a string option",                                      \
                          opt->type == VMAF_OPT_TYPE_STRING);                                        \
            }                                                                                        \
        }                                                                                            \
        mu_assert(ext_name " must expose a 'model_path' option", saw_model_path);                    \
        return NULL;                                                                                 \
    }                                                                                                \
                                                                                                     \
    static mu_message_t test_##fn_prefix##_init_rejects_missing_model(void)                          \
    {                                                                                                \
        /* With neither model_path option set nor env var in the                                   \
         * environment, init() must cleanly decline rather than crash. */ \
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(ext_name);                    \
        mu_assert(ext_name " extractor missing", fex != NULL);                                       \
                                                                                                     \
        void *priv = calloc(1, fex->priv_size);                                                      \
        mu_assert("priv alloc failed", priv != NULL);                                                \
        fex->priv = priv;                                                                            \
                                                                                                     \
        /* Save + clear the env var for the duration of this test. */                                \
        const char *saved = getenv(env_var);                                                         \
        char *saved_copy = saved ? strdup(saved) : NULL;                                             \
        (void)vmaf_tiny_ai_test_unsetenv(env_var);                                                   \
                                                                                                     \
        int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, 64u, 64u);                                 \
        mu_assert("init must fail when no model path is provided", rc < 0);                          \
        if (vmaf_dnn_available()) {                                                                  \
            mu_assert("DNN-enabled build must report missing model path as -EINVAL",                 \
                      rc == -EINVAL);                                                                \
        } else {                                                                                     \
            mu_assert("DNN-disabled build must report optional runtime as -ENOSYS",                  \
                      rc == -ENOSYS);                                                                \
        }                                                                                            \
                                                                                                     \
        /* close() must be safe after a failed init. */                                              \
        (void)fex->close(fex);                                                                       \
                                                                                                     \
        if (saved_copy) {                                                                            \
            (void)vmaf_tiny_ai_test_setenv(env_var, saved_copy);                                     \
            free(saved_copy);                                                                        \
        }                                                                                            \
        free(priv);                                                                                  \
        fex->priv = NULL;                                                                            \
        return NULL;                                                                                 \
    }

/**
 * Convenience: run all four standard tests via `mu_run_test`. Caller
 * may append extractor-specific `mu_run_test()` calls afterwards.
 */
#define VMAF_TINY_AI_RUN_REGISTRATION_TESTS(fn_prefix)                                             \
    do {                                                                                           \
        mu_run_test(test_##fn_prefix##_is_registered);                                             \
        mu_run_test(test_##fn_prefix##_provides_primary_feature);                                  \
        mu_run_test(test_##fn_prefix##_options_table_well_formed);                                 \
        mu_run_test(test_##fn_prefix##_init_rejects_missing_model);                                \
    } while (0)

#endif /* LIBVMAF_TEST_TINY_AI_TEST_TEMPLATE_H_ */
