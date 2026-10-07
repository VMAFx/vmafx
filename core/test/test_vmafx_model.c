/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx models and model sets (ADR-1852, RC4 WP2): loading built-ins and
 * files, the SHA-256 of the bytes as loaded, feature overrides on an unshared
 * model only, reference counts, the built-in list. Each function: success,
 * every named failure, NULL error pointer.
 *
 * The expected hashes are `sha256sum model/vmaf_v0.6.1.json` and
 * `sha256sum model/vmaf_b_v0.6.3.json`; a built-in model is the xxd -i image
 * of the same file, so both loads must report it.
 *
 * Failing first: the functions do not exist on the WP1 base. Measured with
 * planted defects on this branch: hashing the parsed model instead of the
 * loaded bytes, or the file path instead of its contents, fails
 * test_hash_equals_file_digest; an override allowed on a shared model fails
 * test_override_feature.
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
#define SHA256_VMAF_B_V063 "34f620dbaff662fe7d33dd80326553c2cc3910e4135349ba4c367171d3a3cfbd"

#ifndef VMAFX_TEST_MODEL_DIR
#error "VMAFX_TEST_MODEL_DIR: the model directory, set by core/test/meson.build"
#endif

/* `name` in the source tree's model directory. */
static const char *model_path(const char *name)
{
    static char path[4096];
    const int n = snprintf(path, sizeof(path), "%s/%s", VMAFX_TEST_MODEL_DIR, name);
    return n > 0 && (size_t)n < sizeof(path) ? path : "";
}

static VmafxModel *builtin(const char *version)
{
    VmafxModel *model = NULL;
    return vmafx_model_load(NULL, version, &model, NULL) == VMAFX_OK ? model : NULL;
}

/* ---- Loading ----------------------------------------------------------------------- */

static char *test_load_builtin(void)
{
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    mu_assert("load", vmafx_model_load(NULL, "vmaf_v0.6.1", &model, &error) == VMAFX_OK);
    mu_assert("name", !strcmp(vmafx_model_name(model), "vmaf"));
    const uint32_t n = vmafx_model_feature_count(model);
    mu_assert("features", n > 0 && vmafx_model_feature_name(model, 0) != NULL &&
                              vmafx_model_feature_name(model, n) == NULL);
    VmafxModelConfig config = VMAFX_MODEL_CONFIG_INIT;
    config.name = "custom";
    VmafxModel *named = NULL;
    mu_assert("named", vmafx_model_load(&config, "vmaf_v0.6.1", &named, NULL) == VMAFX_OK &&
                           !strcmp(vmafx_model_name(named), "custom"));
    vmafx_model_unref(named);
    vmafx_model_unref(model);
    return NULL;
}

static char *test_load_failures(void)
{
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    mu_assert("unknown version",
              vmafx_model_load(NULL, "no_such", &model, &error) == VMAFX_E_NOTFOUND);
    mu_assert("named",
              vt_failed(&error, VMAFX_E_NOTFOUND, "no_such", VMAFX_SUBJECT_MODEL) && !model);
    mu_assert("a set is not a model",
              vmafx_model_load(NULL, "vmaf_b_v0.6.3", &model, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "vmaf_b_v0.6.3", VMAFX_SUBJECT_MODEL));
    mu_assert("NULL version",
              vmafx_model_load(NULL, NULL, &model, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "version", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out", vmafx_model_load(NULL, "vmaf_v0.6.1", NULL, &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL error pointer",
              vmafx_model_load(NULL, "no_such", &model, NULL) == VMAFX_E_NOTFOUND);
    return NULL;
}

static char *test_load_config_failures(void)
{
    VmafxModelConfig config = VMAFX_MODEL_CONFIG_INIT;
    config.flags = UINT64_C(1) << 40;
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    mu_assert("unknown flag",
              vmafx_model_load(&config, "vmaf_v0.6.1", &model, &error) == VMAFX_E_INVALID);
    mu_assert("named", vt_failed(&error, VMAFX_E_INVALID, "config.flags", VMAFX_SUBJECT_PARAMETER));
    config.flags = VMAFX_MODEL_DISABLE_CLIP;
    config.struct_size = 4;
    mu_assert("short config",
              vmafx_model_load(&config, "vmaf_v0.6.1", &model, &error) == VMAFX_E_ABI &&
                  vt_failed(&error, VMAFX_E_ABI, "config", VMAFX_SUBJECT_PARAMETER));
    config.struct_size = (uint32_t)sizeof(config);
    mu_assert("known flag", vmafx_model_load(&config, "vmaf_v0.6.1", &model, NULL) == VMAFX_OK);
    vmafx_model_unref(model);
    return NULL;
}

static char *test_load_file_failures(void)
{
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    const char *missing = model_path("no_such_model.json");
    mu_assert("missing file", vmafx_model_load_file(NULL, missing, &model, &error) == VMAFX_E_IO &&
                                  vt_failed(&error, VMAFX_E_IO, missing, VMAFX_SUBJECT_PATH));
    const char *pkl = model_path("vmaf_v0.6.1.pkl");
    mu_assert("pkl", vmafx_model_load_file(NULL, pkl, &model, &error) == VMAFX_E_NOTSUP &&
                         vt_failed(&error, VMAFX_E_NOTSUP, pkl, VMAFX_SUBJECT_PATH));
    const char *set = model_path("vmaf_b_v0.6.3.json");
    mu_assert("a set file is not a model",
              vmafx_model_load_file(NULL, set, &model, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, set, VMAFX_SUBJECT_MODEL));
    const char *dir = VMAFX_TEST_MODEL_DIR;
    mu_assert("a directory", vmafx_model_load_file(NULL, dir, &model, &error) == VMAFX_E_IO &&
                                 vt_failed(&error, VMAFX_E_IO, dir, VMAFX_SUBJECT_PATH));
    mu_assert("NULL path", vmafx_model_load_file(NULL, NULL, &model, &error) == VMAFX_E_INVALID &&
                               vt_failed(&error, VMAFX_E_INVALID, "path", VMAFX_SUBJECT_PARAMETER));
    mu_assert("nothing loaded", model == NULL);
    return NULL;
}

/* ---- Hashes ------------------------------------------------------------------------ */

static char *test_hash_equals_file_digest(void)
{
    VmafxModel *embedded = builtin("vmaf_v0.6.1");
    VmafxModel *file = NULL;
    mu_assert("file",
              vmafx_model_load_file(NULL, model_path("vmaf_v0.6.1.json"), &file, NULL) == VMAFX_OK);
    mu_assert("built-in hash", embedded && !strcmp(vmafx_model_hash(embedded), SHA256_VMAF_V061));
    mu_assert("file hash", !strcmp(vmafx_model_hash(file), SHA256_VMAF_V061));
    mu_assert("same features",
              vmafx_model_feature_count(file) == vmafx_model_feature_count(embedded));
    vmafx_model_unref(file);
    vmafx_model_unref(embedded);
    mu_assert("NULL queries", !vmafx_model_hash(NULL) && !vmafx_model_name(NULL) &&
                                  vmafx_model_feature_count(NULL) == 0 &&
                                  !vmafx_model_feature_name(NULL, 0));
    return NULL;
}

/* ---- Overrides and references -------------------------------------------------------- */

static char *test_override_feature(void)
{
    VmafxModel *model = builtin("vmaf_v0.6.1");
    VmafxOptions *options = NULL;
    mu_assert("options", vmafx_options_set(&options, "adm_norm_view_dist", "1.5", NULL) == 0);
    VmafxError *error = NULL;
    mu_assert("unshared", vmafx_model_override_feature(model, "adm", options, &error) == VMAFX_OK);
    mu_assert("an extractor the model does not read",
              vmafx_model_override_feature(model, "psnr", options, &error) == VMAFX_E_NOTFOUND &&
                  vt_failed(&error, VMAFX_E_NOTFOUND, "psnr", VMAFX_SUBJECT_EXTRACTOR));
    VmafxModel *second = vmafx_model_ref(model);
    mu_assert("shared",
              vmafx_model_override_feature(model, "adm", options, &error) == VMAFX_E_BUSY);
    mu_assert("named", vt_failed(&error, VMAFX_E_BUSY, "vmaf", VMAFX_SUBJECT_MODEL));
    vmafx_model_unref(second);
    mu_assert("NULL options",
              vmafx_model_override_feature(model, "adm", NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "options", VMAFX_SUBJECT_PARAMETER));
    vmafx_options_free(options);
    vmafx_model_unref(model);
    return NULL;
}

static char *test_references(void)
{
    VmafxModel *model = builtin("vmaf_v0.6.1");
    mu_assert("ref returns the model", vmafx_model_ref(model) == model);
    vmafx_model_unref(model);
    mu_assert("still loaded after one unref", vmafx_model_feature_count(model) > 0);
    vmafx_model_unref(model);
    mu_assert("NULL", vmafx_model_ref(NULL) == NULL);
    vmafx_model_unref(NULL);
    return NULL;
}

static char *test_builtin_list(void)
{
    unsigned count = 0;
    bool default_listed = false;
    const char *version = vmafx_model_builtin_next(NULL);
    for (; version && count < 1000u; version = vmafx_model_builtin_next(version)) {
        default_listed |= !strcmp(version, vmafx_model_default_version());
        count++;
    }
    mu_assert("listed", count >= 5 && default_listed);
    mu_assert("default", !strcmp(vmafx_model_default_version(), "vmaf_v1.0.16_3d0h"));
    mu_assert("unknown previous", vmafx_model_builtin_next("no_such") == NULL);
    return NULL;
}

/* ---- Model sets --------------------------------------------------------------------- */

static char *test_model_set_load(void)
{
    VmafxModelSet *set = NULL;
    VmafxError *error = NULL;
    mu_assert("load", vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", &set, &error) == VMAFX_OK);
    mu_assert("members and lead", vmafx_model_set_size(set) > 0 && vmafx_model_set_lead(set));
    mu_assert("hashes",
              !strcmp(vmafx_model_set_hash(set), SHA256_VMAF_B_V063) &&
                  !strcmp(vmafx_model_hash(vmafx_model_set_lead(set)), SHA256_VMAF_B_V063));
    VmafxModelSet *file = NULL;
    mu_assert("file", vmafx_model_set_load_file(NULL, model_path("vmaf_b_v0.6.3.json"), &file,
                                                NULL) == VMAFX_OK &&
                          !strcmp(vmafx_model_set_hash(file), SHA256_VMAF_B_V063));
    mu_assert("ref", vmafx_model_set_ref(file) == file);
    vmafx_model_set_unref(file);
    vmafx_model_set_unref(file);
    vmafx_model_set_unref(set);
    mu_assert("NULL queries", !vmafx_model_set_lead(NULL) && vmafx_model_set_size(NULL) == 0 &&
                                  !vmafx_model_set_hash(NULL) && !vmafx_model_set_ref(NULL));
    vmafx_model_set_unref(NULL);
    return NULL;
}

static char *test_model_set_failures(void)
{
    VmafxModelSet *set = NULL;
    VmafxError *error = NULL;
    mu_assert("a model is not a set",
              vmafx_model_set_load(NULL, "vmaf_v0.6.1", &set, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "vmaf_v0.6.1", VMAFX_SUBJECT_MODEL));
    mu_assert("unknown", vmafx_model_set_load(NULL, "no_such", &set, &error) == VMAFX_E_NOTFOUND &&
                             vt_failed(&error, VMAFX_E_NOTFOUND, "no_such", VMAFX_SUBJECT_MODEL));
    const char *missing = model_path("no_such_set.json");
    mu_assert("missing file",
              vmafx_model_set_load_file(NULL, missing, &set, &error) == VMAFX_E_IO &&
                  vt_failed(&error, VMAFX_E_IO, missing, VMAFX_SUBJECT_PATH));
    mu_assert("NULL out",
              vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL error pointer",
              vmafx_model_set_load(NULL, "no_such", &set, NULL) == VMAFX_E_NOTFOUND && set == NULL);
    return NULL;
}

static char *test_model_set_override(void)
{
    VmafxModelSet *set = NULL;
    mu_assert("load", vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", &set, NULL) == VMAFX_OK);
    VmafxOptions *options = NULL;
    mu_assert("options", vmafx_options_set(&options, "adm_norm_view_dist", "1.5", NULL) == 0);
    VmafxError *error = NULL;
    mu_assert("unshared",
              vmafx_model_set_override_feature(set, "adm", options, &error) == VMAFX_OK);
    VmafxModel *lead = vmafx_model_ref(vmafx_model_set_lead(set));
    mu_assert("lead shared",
              vmafx_model_set_override_feature(set, "adm", options, &error) == VMAFX_E_BUSY &&
                  vt_failed(&error, VMAFX_E_BUSY, "vmaf", VMAFX_SUBJECT_MODEL));
    vmafx_model_unref(lead);
    mu_assert("NULL set",
              vmafx_model_set_override_feature(NULL, "adm", options, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "set", VMAFX_SUBJECT_PARAMETER));
    vmafx_options_free(options);
    vmafx_model_set_unref(set);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_load_builtin),
        MU_TEST(test_load_failures),
        MU_TEST(test_load_config_failures),
        MU_TEST(test_load_file_failures),
        MU_TEST(test_hash_equals_file_digest),
        MU_TEST(test_override_feature),
        MU_TEST(test_references),
        MU_TEST(test_builtin_list),
        MU_TEST(test_model_set_load),
        MU_TEST(test_model_set_failures),
        MU_TEST(test_model_set_override),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
