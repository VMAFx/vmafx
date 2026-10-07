/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Specification strings of the filters (RC4 WP9): vmafx_model_load_spec() and
 * vmafx_context_use_feature_spec(), the `model` and `feature` option values of
 * the FFmpeg filter and the GStreamer element. Each function: the accepted
 * forms, every named refusal, the length and item bounds.
 *
 * Failing first: the functions do not exist on rc4/integration. Measured with
 * planted defects on this branch: an unescaped value (a backslash kept before
 * `:`) fails test_model_spec_escapes; upstream FFmpeg's `name=` spelling read
 * as an extractor called `name` fails test_feature_spec_forms.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define SHA256_VMAF_V061 "5950d61fa1f861bd45d8149d80539ed9f3376cfc2495b8f0fa8e9f57cb131ee3"

#ifndef VMAFX_TEST_MODEL_DIR
#error "VMAFX_TEST_MODEL_DIR: the model directory, set by core/test/meson.build"
#endif

/* `spec` loaded, or NULL. */
static VmafxModel *spec_model(const char *spec)
{
    VmafxModel *model = NULL;
    return vmafx_model_load_spec(NULL, spec, &model, NULL) == VMAFX_OK ? model : NULL;
}

static bool model_is(VmafxModel *model, const char *name, const char *hash)
{
    const bool ok = model && strcmp(vmafx_model_name(model), name) == 0 &&
                    (!hash || strcmp(vmafx_model_hash(model), hash) == 0);
    vmafx_model_unref(model);
    return ok;
}

/* `spec` refused with `status` naming `subject` of `kind`. */
static bool model_refused(const char *spec, VmafxStatus status, const char *subject, uint32_t kind)
{
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    const VmafxStatus got = vmafx_model_load_spec(NULL, spec, &model, &error);
    return got == status && model == NULL && vt_failed(&error, status, subject, kind);
}

static char *test_model_spec_forms(void)
{
    VmafxModel *def = NULL;
    mu_assert("default",
              vmafx_model_load(NULL, vmafx_model_default_version(), &def, NULL) == VMAFX_OK);
    char hash[80];
    (void)snprintf(hash, sizeof(hash), "%s", vmafx_model_hash(def));
    vmafx_model_unref(def);
    mu_assert("empty: the default model", model_is(spec_model(""), "vmaf", hash));
    mu_assert("version", model_is(spec_model("version=vmaf_v0.6.1"), "vmaf", SHA256_VMAF_V061));
    char spec[4200];
    (void)snprintf(spec, sizeof(spec), "path=%s/vmaf_v0.6.1.json:name=file_model",
                   VMAFX_TEST_MODEL_DIR);
    mu_assert("path and name", model_is(spec_model(spec), "file_model", SHA256_VMAF_V061));
    mu_assert("flags without and with a value",
              model_is(spec_model("version=vmaf_v0.6.1:disable_clip:enable_transform=true"), "vmaf",
                       SHA256_VMAF_V061));
    mu_assert("an override", model_is(spec_model("version=vmaf_v0.6.1:adm.adm_enhn_gain_limit=1.0"),
                                      "vmaf", SHA256_VMAF_V061));
    VmafxModelConfig config = VMAFX_MODEL_CONFIG_INIT;
    config.name = "from_config";
    VmafxModel *model = NULL;
    mu_assert("config name as the default",
              vmafx_model_load_spec(&config, "version=vmaf_v0.6.1", &model, NULL) == VMAFX_OK &&
                  model_is(model, "from_config", SHA256_VMAF_V061));
    return NULL;
}

static char *test_model_spec_escapes(void)
{
    mu_assert("escaped colon and equals in a name",
              model_is(spec_model("version=vmaf_v0.6.1:name=a\\:b\\=c"), "a:b=c", NULL));
    mu_assert("a backslash before anything else is data",
              model_is(spec_model("version=vmaf_v0.6.1:name=C\\models"), "C\\models", NULL));
    return NULL;
}

static char *test_model_spec_refusals(void)
{
    mu_assert("NULL spec", model_refused(NULL, VMAFX_E_INVALID, "spec", VMAFX_SUBJECT_PARAMETER));
    VmafxError *error = NULL;
    mu_assert("NULL out", vmafx_model_load_spec(NULL, "", NULL, &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    mu_assert("both version and path",
              model_refused("version=vmaf_v0.6.1:path=x.json", VMAFX_E_INVALID, "path",
                            VMAFX_SUBJECT_OPTION));
    return NULL;
}

static char *test_model_spec_refused_items(void)
{
    mu_assert("a key without a value",
              model_refused("version", VMAFX_E_INVALID, "version", VMAFX_SUBJECT_OPTION));
    mu_assert("a flag with another value", model_refused("disable_clip=maybe", VMAFX_E_INVALID,
                                                         "disable_clip", VMAFX_SUBJECT_OPTION));
    mu_assert(
        "an override without an option",
        model_refused("version=vmaf_v0.6.1:adm.=1", VMAFX_E_INVALID, "adm", VMAFX_SUBJECT_OPTION));
    mu_assert("an override of an extractor the model does not read",
              model_refused("version=vmaf_v0.6.1:psnr.enable_mse=true", VMAFX_E_NOTFOUND, "psnr",
                            VMAFX_SUBJECT_EXTRACTOR));
    mu_assert("an unknown version", model_refused("version=no_such_model", VMAFX_E_NOTFOUND,
                                                  "no_such_model", VMAFX_SUBJECT_MODEL));
    return NULL;
}

/* `n` items of `name=x`; the first `version=vmaf_v0.6.1`. */
static char *items_spec(unsigned n)
{
    char *const spec = calloc(n * 8u + 32u, 1);
    if (!spec) {
        return NULL;
    }
    static const char first[] = "version=vmaf_v0.6.1";
    memcpy(spec, first, sizeof(first));
    size_t len = sizeof(first) - 1u;
    for (unsigned i = 1; i < n; i++) {
        memcpy(spec + len, ":name=x", 8u);
        len += 7u;
    }
    return spec;
}

static char *test_model_spec_bounds(void)
{
    char *const at_limit = items_spec(64u);
    char *const past_limit = items_spec(65u);
    mu_assert("allocation", at_limit && past_limit);
    mu_assert("64 items", model_is(spec_model(at_limit), "x", SHA256_VMAF_V061));
    mu_assert("65 items",
              model_refused(past_limit, VMAFX_E_RANGE, "spec", VMAFX_SUBJECT_PARAMETER));
    free(at_limit);
    free(past_limit);
    char long_spec[4098];
    memset(long_spec, 'x', sizeof(long_spec) - 1u);
    memcpy(long_spec, "version=vmaf_v0.6.1:name=", 25u);
    long_spec[4096] = '\0';
    mu_assert("4096 bytes", model_is(spec_model(long_spec), long_spec + 25, SHA256_VMAF_V061));
    long_spec[4096] = 'x';
    long_spec[4097] = '\0';
    mu_assert("4097 bytes",
              model_refused(long_spec, VMAFX_E_RANGE, "spec", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

/* The name of the only extractor `spec` registers on a fresh context, or
 * NULL; `status` receives the call's status. */
static const char *registered(const char *spec, VmafxStatus *status, VmafxError **error)
{
    static char name[64];
    VmafxContext *context = NULL;
    name[0] = '\0';
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        *status = VMAFX_E_NOMEM;
        return NULL;
    }
    *status = vmafx_context_use_feature_spec(context, spec, error);
    VmafxExtractorInfo info = VMAFX_EXTRACTOR_INFO_INIT;
    if (*status == VMAFX_OK && vmafx_context_extractor_count(context) == 1u &&
        vmafx_context_extractor_info(context, 0, &info, NULL) == VMAFX_OK) {
        (void)snprintf(name, sizeof(name), "%s", info.name);
    }
    (void)vmafx_context_destroy(context, NULL);
    return name[0] ? name : NULL;
}

static bool feature_is(const char *spec, const char *expected)
{
    VmafxStatus status = VMAFX_OK;
    const char *const name = registered(spec, &status, NULL);
    return status == VMAFX_OK && name && strcmp(name, expected) == 0;
}

static bool feature_refused(const char *spec, VmafxStatus expected, const char *subject,
                            uint32_t kind)
{
    VmafxStatus status = VMAFX_OK;
    VmafxError *error = NULL;
    (void)registered(spec, &status, &error);
    return status == expected && vt_failed(&error, expected, subject, kind);
}

static char *test_feature_spec_forms(void)
{
    mu_assert("a name", feature_is("psnr", "psnr"));
    mu_assert("VMAFx form", feature_is("psnr=enable_chroma=false:enable_mse=true", "psnr"));
    mu_assert("upstream FFmpeg form", feature_is("name=psnr:enable_chroma=false", "psnr"));
    mu_assert("upstream FFmpeg form, name only", feature_is("name=float_ssim", "float_ssim"));
    mu_assert("CLI alias", feature_is("integer_psnr", "psnr"));
    return NULL;
}

static char *test_feature_spec_refusals(void)
{
    VmafxError *error = NULL;
    mu_assert("NULL context",
              vmafx_context_use_feature_spec(NULL, "psnr", &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "context", VMAFX_SUBJECT_PARAMETER));
    mu_assert("empty", feature_refused("", VMAFX_E_INVALID, "spec", VMAFX_SUBJECT_PARAMETER));
    mu_assert("no extractor name",
              feature_refused("=x=1", VMAFX_E_INVALID, "spec", VMAFX_SUBJECT_OPTION));
    mu_assert("an option without a value", feature_refused("psnr:enable_mse", VMAFX_E_INVALID,
                                                           "enable_mse", VMAFX_SUBJECT_OPTION));
    mu_assert("an unknown extractor",
              feature_refused("no_such_extractor", VMAFX_E_NOTFOUND, "no_such_extractor",
                              VMAFX_SUBJECT_EXTRACTOR));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_model_spec_forms),      MU_TEST(test_model_spec_escapes),
        MU_TEST(test_model_spec_refusals),   MU_TEST(test_model_spec_refused_items),
        MU_TEST(test_model_spec_bounds),     MU_TEST(test_feature_spec_forms),
        MU_TEST(test_feature_spec_refusals),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
