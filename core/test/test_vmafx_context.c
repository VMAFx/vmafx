/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx contexts, errors, logging, options, registration and struct size
 * negotiation (ADR-1852, RC4 WP2). Each function: success, every named
 * failure (subject and subject kind asserted), and the NULL error pointer.
 *
 * Failing first: none of these functions exists on the WP1 base, so this test
 * does not link there. Measured on this branch with planted defects: a log
 * sink not installed around engine calls fails test_engine_messages_reach_callback;
 * a context that sets the process level with a callback fails
 * test_callback_context_keeps_process_level; vmafx_read_sized() reading past
 * the caller's struct_size crashes test_config_older_struct (garbage
 * callback pointer).
 */

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf_priv.h"
#include "log.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static VmafxContext *plain_context(void)
{
    VmafxContext *context = NULL;
    return vmafx_context_create(NULL, &context, NULL) == VMAFX_OK ? context : NULL;
}

/* ---- Errors ---------------------------------------------------------------------- */

static char *test_error_accessors_of_null(void)
{
    mu_assert("status", vmafx_error_status(NULL) == VMAFX_E_INVALID);
    mu_assert("texts", !strcmp(vmafx_error_message(NULL), "") &&
                           !strcmp(vmafx_error_subject(NULL), "") &&
                           !strcmp(vmafx_error_function(NULL), ""));
    mu_assert("kind and errno",
              vmafx_error_subject_kind(NULL) == VMAFX_SUBJECT_NONE && vmafx_error_errno(NULL) == 0);
    vmafx_error_free(NULL);
    mu_assert("status names", !strcmp(vmafx_status_name(VMAFX_E_ABI), "VMAFX_E_ABI") &&
                                  !strcmp(vmafx_status_name(12345), "VMAFX_UNKNOWN_STATUS"));
    return NULL;
}

static char *test_failure_names_function_and_kind(void)
{
    VmafxContext *context = plain_context();
    mu_assert("context", context != NULL);
    VmafxError *error = NULL;
    mu_assert("unknown option",
              vmafx_context_set_option(context, "no_such_option", "1", &error) == VMAFX_E_NOTFOUND);
    mu_assert("function", !strcmp(vmafx_error_function(error), "vmafx_context_set_option"));
    mu_assert("subject",
              vt_failed(&error, VMAFX_E_NOTFOUND, "no_such_option", VMAFX_SUBJECT_OPTION));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Logging --------------------------------------------------------------------- */

static char *test_unclaimed_failure_reaches_callback(void)
{
    VtLog log = {0};
    VmafxContext *context = vt_logged_context(&log, VMAFX_LOG_LEVEL_NONE);
    mu_assert("context", context != NULL);
    /* No error out-parameter: the failure is delivered at ERROR whatever the
     * level (design section 2.5), naming the function and the subject. */
    mu_assert("failure", vmafx_context_set_option(context, "bogus", "1", NULL) == VMAFX_E_NOTFOUND);
    mu_assert("delivered once at ERROR", log.count == 1 && log.errors == 1);
    mu_assert("names it",
              strstr(log.last, "vmafx_context_set_option") && strstr(log.last, "bogus"));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* An option the extractor does not know: the engine logs it on the calling
 * thread during the call. */
static VmafxStatus use_psnr_with_bad_option(VmafxContext *context, VmafxError **error)
{
    VmafxOptions *options = NULL;
    if (vmafx_options_set(&options, "no_such_psnr_option", "1", NULL) != VMAFX_OK) {
        return VMAFX_E_NOMEM;
    }
    const VmafxStatus status = vmafx_context_use_feature(context, "psnr", options, error);
    vmafx_options_free(options);
    return status;
}

static char *test_engine_messages_reach_callback(void)
{
    VtLog log = {0};
    VmafxContext *context = vt_logged_context(&log, VMAFX_LOG_LEVEL_ERROR);
    mu_assert("context", context != NULL);
    VmafxError *error = NULL;
    mu_assert("refused", use_psnr_with_bad_option(context, &error) == VMAFX_E_INVALID);
    mu_assert("named", vt_failed(&error, VMAFX_E_INVALID, "psnr", VMAFX_SUBJECT_EXTRACTOR));
    mu_assert("the engine's message reached the callback",
              log.count == 1 && strstr(log.last, "no_such_psnr_option") != NULL);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_callback_level_filters_engine_messages(void)
{
    VtLog log = {0};
    VmafxContext *context = vt_logged_context(&log, VMAFX_LOG_LEVEL_NONE);
    mu_assert("context", context != NULL);
    VmafxError *error = NULL;
    mu_assert("refused", use_psnr_with_bad_option(context, &error) == VMAFX_E_INVALID);
    vmafx_error_free(error);
    mu_assert("level NONE drops the engine's message", log.count == 0);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_callback_context_keeps_process_level(void)
{
    vmaf_set_log_level(VMAF_LOG_LEVEL_WARNING);
    VtLog log = {0};
    VmafxContext *logged = vt_logged_context(&log, VMAFX_LOG_LEVEL_DEBUG);
    mu_assert("logged context", logged != NULL);
    mu_assert("a callback context leaves the process level",
              vmaf_get_log_level() == VMAF_LOG_LEVEL_WARNING);
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.log_level = VMAFX_LOG_LEVEL_ERROR;
    VmafxContext *plain = NULL;
    mu_assert("plain context", vmafx_context_create(&config, &plain, NULL) == VMAFX_OK);
    mu_assert("a plain context sets it, as vmaf_init() does",
              vmaf_get_log_level() == VMAF_LOG_LEVEL_ERROR);
    mu_assert("destroy", vmafx_context_destroy(logged, NULL) == VMAFX_OK &&
                             vmafx_context_destroy(plain, NULL) == VMAFX_OK);
    vmaf_set_log_level(VMAF_LOG_LEVEL_INFO);
    return NULL;
}

/* ---- Struct size negotiation ----------------------------------------------------- */

/* A caller struct of `size` bytes inside a buffer whose tail is garbage. */
typedef union ConfigBuffer {
    VmafxContextConfig config;
    unsigned char bytes[sizeof(VmafxContextConfig) + 32];
} ConfigBuffer;

static char *test_config_older_struct(void)
{
    ConfigBuffer buffer;
    memset(buffer.bytes, 0xa5, sizeof(buffer.bytes));
    /* ABI 0.1.0 ended at gpumask: log_callback / log_user are garbage here and
     * must not be read. */
    const uint32_t old_size = (uint32_t)offsetof(VmafxContextConfig, log_callback);
    memset(buffer.bytes, 0, old_size);
    buffer.config.struct_size = old_size;
    VmafxContext *context = NULL;
    mu_assert("older struct accepted",
              vmafx_context_create(&buffer.config, &context, NULL) == VMAFX_OK);
    mu_assert("no callback was read",
              vmafx_context_set_option(context, "x", "1", NULL) == VMAFX_E_NOTFOUND);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_config_newer_struct(void)
{
    ConfigBuffer buffer;
    memset(buffer.bytes, 0x5a, sizeof(buffer.bytes));
    VmafxContextConfig defaults = VMAFX_CONTEXT_CONFIG_INIT;
    buffer.config = defaults;
    buffer.config.struct_size = (uint32_t)sizeof(buffer.bytes); /* fields we do not know */
    VmafxContext *context = NULL;
    mu_assert("newer struct accepted",
              vmafx_context_create(&buffer.config, &context, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_config_too_short(void)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.struct_size = (uint32_t)offsetof(VmafxContextConfig, gpumask);
    VmafxContext *context = NULL;
    VmafxError *error = NULL;
    mu_assert("refused", vmafx_context_create(&config, &context, &error) == VMAFX_E_ABI);
    mu_assert("named",
              vt_failed(&error, VMAFX_E_ABI, "config", VMAFX_SUBJECT_PARAMETER) && context == NULL);
    mu_assert("NULL error pointer",
              vmafx_context_create(&config, &context, NULL) == VMAFX_E_ABI && context == NULL);
    return NULL;
}

/* An output struct of a newer caller: bytes past ours are not touched. */
typedef union InfoBuffer {
    VmafxExtractorInfo info;
    unsigned char bytes[sizeof(VmafxExtractorInfo) + 16];
} InfoBuffer;

static char *test_output_struct_sizes(void)
{
    VmafxContext *context = plain_context();
    mu_assert("psnr", context && vmafx_context_use_feature(context, "psnr", NULL, NULL) == 0);
    InfoBuffer newer;
    memset(newer.bytes, 0x77, sizeof(newer.bytes));
    newer.info.struct_size = (uint32_t)sizeof(newer.bytes);
    mu_assert("newer", vmafx_context_extractor_info(context, 0, &newer.info, NULL) == VMAFX_OK);
    mu_assert("written size", newer.info.struct_size == sizeof(VmafxExtractorInfo) &&
                                  newer.bytes[sizeof(newer.bytes) - 1] == 0x77);
    VmafxExtractorInfo older = VMAFX_EXTRACTOR_INFO_INIT;
    older.struct_size = (uint32_t)offsetof(VmafxExtractorInfo, name);
    older.name = "untouched";
    mu_assert("older", vmafx_context_extractor_info(context, 0, &older, NULL) == VMAFX_OK &&
                           !strcmp(older.name, "untouched"));
    older.struct_size = 2;
    VmafxError *error = NULL;
    mu_assert("below struct_size",
              vmafx_context_extractor_info(context, 0, &older, &error) == VMAFX_E_ABI);
    mu_assert("named", vt_failed(&error, VMAFX_E_ABI, "out", VMAFX_SUBJECT_PARAMETER));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Options --------------------------------------------------------------------- */

static char *test_options_set_and_free(void)
{
    VmafxOptions *options = NULL;
    mu_assert("first key creates the set",
              vmafx_options_set(&options, "enable_chroma", "false", NULL) == VMAFX_OK && options);
    mu_assert("second key", vmafx_options_set(&options, "min_sse", "0.5", NULL) == VMAFX_OK);
    VmafxContext *context = plain_context();
    mu_assert("options reach the extractor",
              context && vmafx_context_use_feature(context, "psnr", options, NULL) == VMAFX_OK);
    vmafx_options_free(options); /* the context keeps its own copy */
    mu_assert("one extractor", vmafx_context_extractor_count(context) == 1);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    vmafx_options_free(NULL);
    return NULL;
}

static char *test_options_set_failures(void)
{
    VmafxOptions *options = NULL;
    VmafxError *error = NULL;
    mu_assert("NULL set",
              vmafx_options_set(NULL, "k", "v", &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "options", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL key", vmafx_options_set(&options, NULL, "v", &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "key", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL value",
              vmafx_options_set(&options, "k", NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "value", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL error pointer", vmafx_options_set(NULL, "k", "v", NULL) == VMAFX_E_INVALID);
    mu_assert("nothing created", options == NULL);
    return NULL;
}

/* ---- Context options ------------------------------------------------------------- */

typedef struct OptionCase {
    const char *key;
    const char *value;
    VmafxStatus status;
} OptionCase;

static const OptionCase option_cases[] = {
    {"perceptual_weight", "1", VMAFX_OK},
    {"perceptual_weight", "false", VMAFX_OK},
    {"perceptual_weight", "yes", VMAFX_E_INVALID},
    {"perceptual_weight_strength", "0.75", VMAFX_OK},
    {"perceptual_weight_strength", "-1", VMAFX_E_INVALID},
    {"perceptual_weight_strength", "nan", VMAFX_E_INVALID},
    {"perceptual_weight_strength", "1x", VMAFX_E_INVALID},
    {"perceptual_weight_strength", "", VMAFX_E_INVALID},
};

static char *test_set_option_values(void)
{
    VmafxContext *context = plain_context();
    mu_assert("context", context != NULL);
    for (size_t i = 0; i < sizeof(option_cases) / sizeof(option_cases[0]); i++) {
        const OptionCase *c = &option_cases[i];
        VmafxError *error = NULL;
        const VmafxStatus status = vmafx_context_set_option(context, c->key, c->value, &error);
        mu_assert("status", status == c->status);
        mu_assert("a refusal names the key",
                  status == VMAFX_OK || vt_failed(&error, status, c->key, VMAFX_SUBJECT_OPTION));
    }
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_set_option_null_arguments(void)
{
    VmafxContext *context = plain_context();
    VmafxError *error = NULL;
    mu_assert("NULL context",
              vmafx_context_set_option(NULL, "k", "v", &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "context", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL key", vmafx_context_set_option(context, NULL, "v", &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "key", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL value",
              vmafx_context_set_option(context, "k", NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "value", VMAFX_SUBJECT_PARAMETER));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Registration ----------------------------------------------------------------- */

static char *test_use_feature(void)
{
    VmafxContext *context = plain_context();
    VmafxError *error = NULL;
    mu_assert("psnr", vmafx_context_use_feature(context, "psnr", NULL, &error) == VMAFX_OK);
    VmafxExtractorInfo info = VMAFX_EXTRACTOR_INFO_INIT;
    mu_assert("registered", vmafx_context_extractor_count(context) == 1 &&
                                vmafx_context_extractor_info(context, 0, &info, NULL) == VMAFX_OK &&
                                !strcmp(info.name, "psnr"));
    mu_assert("unknown",
              vmafx_context_use_feature(context, "no_such", NULL, &error) == VMAFX_E_NOTFOUND);
    mu_assert("named", vt_failed(&error, VMAFX_E_NOTFOUND, "no_such", VMAFX_SUBJECT_EXTRACTOR));
    mu_assert("NULL extractor",
              vmafx_context_use_feature(context, NULL, NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "extractor", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL context",
              vmafx_context_use_feature(NULL, "psnr", NULL, NULL) == VMAFX_E_INVALID);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_use_model_and_set(void)
{
    VmafxModel *model = NULL;
    VmafxModelSet *set = NULL;
    mu_assert("load", vmafx_model_load(NULL, "vmaf_v0.6.1", &model, NULL) == VMAFX_OK &&
                          vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", &set, NULL) == VMAFX_OK);
    VmafxContext *context = plain_context();
    mu_assert("use model", vmafx_context_use_model(context, model, NULL) == VMAFX_OK);
    mu_assert("use set", vmafx_context_use_model_set(context, set, NULL) == VMAFX_OK);
    vmafx_model_unref(model); /* the context holds its own references */
    vmafx_model_set_unref(set);
    mu_assert("extractors registered", vmafx_context_extractor_count(context) > 0);
    VmafxError *error = NULL;
    mu_assert("NULL model",
              vmafx_context_use_model(context, NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "model", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL set", vmafx_context_use_model_set(context, NULL, &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "set", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL context", vmafx_context_use_model(NULL, NULL, NULL) == VMAFX_E_INVALID &&
                                  vmafx_context_use_model_set(NULL, NULL, NULL) == VMAFX_E_INVALID);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_import_score(void)
{
    VmafxContext *context = plain_context();
    VmafxError *error = NULL;
    mu_assert("import",
              vmafx_context_import_score(context, "imported", 7, 1.25, &error) == VMAFX_OK);
    VmafxScore score = VMAFX_SCORE_INIT;
    mu_assert("read back", vmafx_feature_score(context, "imported", 7, &score, NULL) == VMAFX_OK &&
                               score.value == 1.25 && score.extractor == NULL);
    const uint64_t too_far = (uint64_t)UINT_MAX + 1u;
    mu_assert("range", vmafx_context_import_score(context, "imported", too_far, 1.0, &error) ==
                           VMAFX_E_RANGE);
    mu_assert("named", vt_failed(&error, VMAFX_E_RANGE, "index", VMAFX_SUBJECT_FRAME));
    mu_assert("engine bound",
              vmafx_context_import_score(context, "imported", 1u << 29, 1.0, &error) ==
                      VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "imported", VMAFX_SUBJECT_FEATURE));
    mu_assert("NULL feature",
              vmafx_context_import_score(context, NULL, 0, 1.0, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "feature", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL context",
              vmafx_context_import_score(NULL, "f", 0, 1.0, NULL) == VMAFX_E_INVALID);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_counts_of_null(void)
{
    mu_assert("extractor count", vmafx_context_extractor_count(NULL) == 0);
    mu_assert("retention", vmafx_context_frame_retention(NULL) == 0);
    return NULL;
}

/* ---- Feature resolution -------------------------------------------------------------- */

static char *test_resolve_on_cpu(void)
{
    VmafxContext *context = plain_context();
    VmafxFeatureResolution r = VMAFX_FEATURE_RESOLUTION_INIT;
    VmafxError *error = NULL;
    mu_assert("psnr", vmafx_feature_resolve(context, "psnr", NULL, NULL, &r, &error) == VMAFX_OK);
    mu_assert("the CPU extractor", r.backend == VMAFX_BACKEND_CPU && !strcmp(r.extractor, "psnr") &&
                                       r.unsupported_option == NULL);
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 64, 64);
    mu_assert("with geometry",
              vmafx_feature_resolve(context, "float_ssim", NULL, &desc, &r, NULL) == VMAFX_OK);
    mu_assert("unknown", vmafx_feature_resolve(context, "no_such", NULL, NULL, &r, &error) ==
                             VMAFX_E_NOTFOUND);
    mu_assert("named", vt_failed(&error, VMAFX_E_NOTFOUND, "no_such", VMAFX_SUBJECT_EXTRACTOR));
    VmafxFrameDesc short_desc = desc;
    short_desc.struct_size = 4;
    mu_assert("short frame desc",
              vmafx_feature_resolve(context, "psnr", NULL, &short_desc, &r, &error) ==
                      VMAFX_E_ABI &&
                  vt_failed(&error, VMAFX_E_ABI, "frame", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out",
              vmafx_feature_resolve(context, "psnr", NULL, NULL, NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* With a device backend (a GPU build), the twin and its refusals. */
static char *test_resolve_device_twin(void)
{
    VmafxContext *context = plain_context();
    static int token;
    const unsigned flag =
        vmaf_context_fake_backend_for_test(vmafx_context_libvmaf_handle(context), &token);
    if (flag) {
        VmafxFeatureResolution r = VMAFX_FEATURE_RESOLUTION_INIT;
        mu_assert("ciede twin",
                  vmafx_feature_resolve(context, "ciede", NULL, NULL, &r, NULL) == VMAFX_OK &&
                      r.backend != VMAFX_BACKEND_CPU && strcmp(r.extractor, "ciede"));
        VmafxError *error = NULL;
        mu_assert("no twin", vmafx_feature_resolve(context, "brisque", NULL, NULL, &r, &error) ==
                                 VMAFX_E_NOTSUP);
        mu_assert("named", vt_failed(&error, VMAFX_E_NOTSUP, "brisque", VMAFX_SUBJECT_EXTRACTOR));
        mu_assert("a twin name is refused",
                  vmafx_feature_resolve(context, r.extractor ? r.extractor : "x", NULL, NULL, &r,
                                        NULL) != VMAFX_OK);
        (void)vmaf_context_fake_backend_for_test(vmafx_context_libvmaf_handle(context), NULL);
    }
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Frame retention (ADR-1478) --------------------------------------------------------- */

static char *test_frame_retention(void)
{
    VmafxContext *context = plain_context();
    mu_assert("psnr", vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    mu_assert("frame n-1", vmafx_context_frame_retention(context) == 1);
    VmafxOptions *five = NULL;
    mu_assert("option",
              vmafx_options_set(&five, "motion_five_frame_window", "true", NULL) == VMAFX_OK);
    mu_assert("motion", vmafx_context_use_feature(context, "motion", five, NULL) == VMAFX_OK);
    vmafx_options_free(five);
    mu_assert("frame n-2", vmafx_context_frame_retention(context) == 2);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_error_accessors_of_null),
        MU_TEST(test_failure_names_function_and_kind),
        MU_TEST(test_unclaimed_failure_reaches_callback),
        MU_TEST(test_engine_messages_reach_callback),
        MU_TEST(test_callback_level_filters_engine_messages),
        MU_TEST(test_callback_context_keeps_process_level),
        MU_TEST(test_config_older_struct),
        MU_TEST(test_config_newer_struct),
        MU_TEST(test_config_too_short),
        MU_TEST(test_output_struct_sizes),
        MU_TEST(test_options_set_and_free),
        MU_TEST(test_options_set_failures),
        MU_TEST(test_set_option_values),
        MU_TEST(test_set_option_null_arguments),
        MU_TEST(test_use_feature),
        MU_TEST(test_use_model_and_set),
        MU_TEST(test_import_score),
        MU_TEST(test_counts_of_null),
        MU_TEST(test_resolve_on_cpu),
        MU_TEST(test_resolve_device_twin),
        MU_TEST(test_frame_retention),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
