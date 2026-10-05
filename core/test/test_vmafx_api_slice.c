/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx API prototype slice (ADR-1852): context create / destroy, version,
 * provenance and extractor queries, one score call, the libvmaf bridge, and
 * the generated libvmaf compat shims (vmaf_init, vmaf_close, vmaf_version,
 * vmaf_feature_score_at_index) that now run on it.
 *
 * Failing first: on master none of the vmafx_ symbols exist and this test does
 * not link. With vmafx_context_create() not binding the engine context to its
 * owner (the link every compat shim relies on), test_create_destroy_defaults
 * fails at "bridge maps back" (measured on the prototype branch).
 */

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
#include "mu_table.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { FRAME_W = 64, FRAME_H = 64 };

static int fill_picture(VmafPicture *pic, unsigned seed)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FRAME_W, FRAME_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; p++) {
        uint8_t *row = pic->data[p];
        for (unsigned y = 0; y < pic->h[p]; y++, row += pic->stride[p]) {
            for (unsigned x = 0; x < pic->w[p]; x++)
                row[x] = (uint8_t)((x * 7u + y * 13u + seed * 29u + p * 3u) & 0xffu);
        }
    }
    return 0;
}

/* One psnr frame through the libvmaf handle bound to `context`. */
static int score_one_frame(VmafxContext *context)
{
    VmafContext *vmaf = vmafx_context_libvmaf_handle(context);
    int err = vmaf_use_feature(vmaf, "psnr", NULL);
    if (err)
        return err;
    VmafPicture ref;
    VmafPicture dist;
    err = fill_picture(&ref, 1);
    if (err)
        return err;
    err = fill_picture(&dist, 2);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0);
    if (err)
        return err;
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* A context that scored one psnr frame, or NULL. */
static VmafxContext *scored_context(void)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK)
        return NULL;
    if (score_one_frame(context) != 0) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* True when the error names `subject`; releases the error. */
static int names_subject(VmafxError *error, const char *subject)
{
    const int named = error && !strcmp(vmafx_error_subject(error), subject) &&
                      strlen(vmafx_error_message(error)) > 0;
    vmafx_error_free(error);
    return named;
}

static char *test_create_destroy_defaults(void)
{
    VmafxContext *context = NULL;
    VmafxError *error = NULL;
    mu_assert("create with NULL config",
              vmafx_context_create(NULL, &context, &error) == VMAFX_OK && context && !error);
    VmafContext *vmaf = vmafx_context_libvmaf_handle(context);
    mu_assert("bridge maps back", vmaf && vmafx_context_from_libvmaf(vmaf) == context);
    mu_assert("destroy", vmafx_context_destroy(context, &error) == VMAFX_OK && !error);
    mu_assert("bridge of NULL",
              !vmafx_context_from_libvmaf(NULL) && !vmafx_context_libvmaf_handle(NULL));
    return NULL;
}

static char *test_create_names_bad_out(void)
{
    VmafxError *error = NULL;
    mu_assert("NULL out", vmafx_context_create(NULL, NULL, &error) == VMAFX_E_INVALID);
    mu_assert("no engine errno", vmafx_error_errno(error) == 0);
    mu_assert("subject out", names_subject(error, "out"));
    mu_assert("NULL error pointer", vmafx_context_create(NULL, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

static char *test_create_names_bad_config(void)
{
    VmafxContext *keep = NULL;
    mu_assert("first context", vmafx_context_create(NULL, &keep, NULL) == VMAFX_OK);
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.struct_size = 8;
    VmafxContext *context = keep; /* a failed create must reset it to NULL */
    VmafxError *error = NULL;
    const VmafxStatus short_struct = vmafx_context_create(&config, &context, &error);
    /* RC4 WP2: below the size the struct had when it was introduced is an ABI
     * mismatch (design section 2.6), naming the struct. */
    mu_assert("short struct", short_struct == VMAFX_E_ABI && context == NULL);
    mu_assert("subject config", names_subject(error, "config"));
    config.struct_size = sizeof(config);
    config.log_level = 99;
    error = NULL;
    mu_assert("bad log level", vmafx_context_create(&config, &context, &error) == VMAFX_E_INVALID);
    mu_assert("subject log_level", names_subject(error, "config.log_level"));
    mu_assert("destroy first", vmafx_context_destroy(keep, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_null_handles(void)
{
    mu_assert("destroy NULL", vmafx_context_destroy(NULL, NULL) == VMAFX_E_INVALID);
    mu_assert("status of NULL", vmafx_error_status(NULL) == VMAFX_E_INVALID);
    mu_assert("text of NULL",
              !strcmp(vmafx_error_message(NULL), "") && !strcmp(vmafx_error_subject(NULL), ""));
    vmafx_error_free(NULL);
    return NULL;
}

static char *test_version_and_abi(void)
{
    const char *version = vmafx_version_string();
    mu_assert("version", version && *version);
    mu_assert("compat vmaf_version is the same string", vmaf_version() == version);
    uint32_t major = 99;
    uint32_t minor = 99;
    uint32_t patch = 99;
    vmafx_abi_version(&major, &minor, &patch);
    mu_assert("abi", major == VMAFX_ABI_VERSION_MAJOR && minor == VMAFX_ABI_VERSION_MINOR &&
                         patch == VMAFX_ABI_VERSION_PATCH);
    vmafx_abi_version(NULL, NULL, NULL);
    mu_assert("status names", !strcmp(vmafx_status_name(VMAFX_E_NOTFOUND), "VMAFX_E_NOTFOUND") &&
                                  !strcmp(vmafx_status_name(12345), "VMAFX_UNKNOWN_STATUS"));
    return NULL;
}

static char *test_provenance(void)
{
    VmafxContext *context = scored_context();
    mu_assert("scored context", context != NULL);
    VmafxProvenance prov = VMAFX_PROVENANCE_INIT;
    mu_assert("provenance", vmafx_context_provenance(context, &prov, NULL) == VMAFX_OK);
    mu_assert("fields", prov.n_extractors == 1 && prov.version == vmafx_version_string() &&
                            prov.active_backend == VMAFX_BACKEND_CPU &&
                            prov.abi_minor == VMAFX_ABI_VERSION_MINOR);
    /* An older caller's shorter struct receives its prefix only. */
    VmafxProvenance older = VMAFX_PROVENANCE_INIT;
    older.struct_size = (uint32_t)offsetof(VmafxProvenance, n_extractors);
    older.n_extractors = 777;
    mu_assert("older struct", vmafx_context_provenance(context, &older, NULL) == VMAFX_OK);
    mu_assert("prefix only", older.n_extractors == 777 &&
                                 older.struct_size == offsetof(VmafxProvenance, n_extractors));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_extractor_info(void)
{
    VmafxContext *context = scored_context();
    mu_assert("scored context", context != NULL);
    VmafxExtractorInfo info = VMAFX_EXTRACTOR_INFO_INIT;
    mu_assert("extractor 0", vmafx_context_extractor_info(context, 0, &info, NULL) == VMAFX_OK &&
                                 !strcmp(info.name, "psnr") && info.backend == VMAFX_BACKEND_CPU);
    VmafxError *error = NULL;
    mu_assert("extractor 1",
              vmafx_context_extractor_info(context, 1, &info, &error) == VMAFX_E_NOTFOUND);
    mu_assert("engine errno", vmafx_error_errno(error) == -ENOENT);
    mu_assert("named index", names_subject(error, "index"));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static int same_bits(double a, double b)
{
    uint64_t x = 0;
    uint64_t y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

static char *test_feature_score_matches_libvmaf(void)
{
    VmafxContext *context = scored_context();
    mu_assert("scored context", context != NULL);
    VmafxScore score = VMAFX_SCORE_INIT;
    mu_assert("score", vmafx_feature_score(context, "psnr_y", 0, &score, NULL) == VMAFX_OK);
    double legacy = 0.0;
    VmafContext *vmaf = vmafx_context_libvmaf_handle(context);
    mu_assert("compat score", vmaf_feature_score_at_index(vmaf, "psnr_y", &legacy, 0) == 0);
    mu_assert("bit-identical", same_bits(legacy, score.value));
    mu_assert("producer", score.extractor && !strcmp(score.extractor, "psnr") &&
                              score.backend == VMAFX_BACKEND_CPU && score.index == 0 &&
                              !strcmp(score.feature, "psnr_y"));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_feature_score_of_imported_value(void)
{
    VmafxContext *context = NULL;
    mu_assert("create", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    VmafContext *vmaf = vmafx_context_libvmaf_handle(context);
    mu_assert("import", vmaf_import_feature_score(vmaf, "imported_feature", 42.5, 3) == 0);
    VmafxScore score = VMAFX_SCORE_INIT;
    mu_assert("imported score",
              vmafx_feature_score(context, "imported_feature", 3, &score, NULL) == VMAFX_OK);
    mu_assert("value, no producer", score.value == 42.5 && score.extractor == NULL);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_feature_score_failures_named(void)
{
    VmafxContext *context = NULL;
    mu_assert("create", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    VmafxScore score = VMAFX_SCORE_INIT;
    VmafxError *error = NULL;
    const uint64_t too_far = (uint64_t)UINT_MAX + 1u;
    mu_assert("range", vmafx_feature_score(context, "x", too_far, &score, &error) == VMAFX_E_RANGE);
    mu_assert("range subject", names_subject(error, "index"));
    /* An unknown feature: the compat shim returns the engine's own errno. */
    error = NULL;
    const VmafxStatus status = vmafx_feature_score(context, "no_such_feature", 0, &score, &error);
    const int32_t engine_errno = vmafx_error_errno(error);
    mu_assert("unknown feature named",
              status < 0 && engine_errno < 0 && names_subject(error, "no_such_feature"));
    double legacy = 0.0;
    VmafContext *vmaf = vmafx_context_libvmaf_handle(context);
    mu_assert("compat errno",
              vmaf_feature_score_at_index(vmaf, "no_such_feature", &legacy, 0) == engine_errno);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_compat_shims_run_on_vmafx(void)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init NULL", vmaf_init(NULL, cfg) == -EINVAL);
    mu_assert("vmaf_init", vmaf_init(&vmaf, cfg) == 0 && vmaf);
    VmafxContext *context = vmafx_context_from_libvmaf(vmaf);
    mu_assert("vmaf_init handle is bound to a VmafxContext",
              context && vmafx_context_libvmaf_handle(context) == vmaf);
    double score = 0.0;
    mu_assert("NULL checks", vmaf_feature_score_at_index(vmaf, NULL, &score, 0) == -EINVAL &&
                                 vmaf_feature_score_at_index(vmaf, "psnr_y", NULL, 0) == -EINVAL);
    mu_assert("vmaf_close", vmaf_close(NULL) == -EINVAL && vmaf_close(vmaf) == 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_create_destroy_defaults),
        MU_TEST(test_create_names_bad_out),
        MU_TEST(test_create_names_bad_config),
        MU_TEST(test_null_handles),
        MU_TEST(test_version_and_abi),
        MU_TEST(test_provenance),
        MU_TEST(test_extractor_info),
        MU_TEST(test_feature_score_matches_libvmaf),
        MU_TEST(test_feature_score_of_imported_value),
        MU_TEST(test_feature_score_failures_named),
        MU_TEST(test_compat_shims_run_on_vmafx),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
