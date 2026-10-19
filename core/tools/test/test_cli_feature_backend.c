/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * ADR-1359: `--feature` routing under an explicit `--backend`. libvmaf is
 * replaced by the fake below, so every outcome of vmaf_feature_backend_twin()
 * is reachable without a GPU. The JSON backend receipt is the library's since
 * RC4 WP5 (core/src/vmafx/provenance_render.c, test_vmafx_provenance).
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cli_feature_backend.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

static const VmafPictureConfiguration pic_1080p = {
    .pic_params = {.w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
    .pic_cnt = 3,
};

/* What the fake lookup was asked. It keeps whether the geometry was the
 * caller's own `pic_1080p`, not the pointer, so no address a caller passes
 * outlives the call: the CLI passes one into its stack-held run state. */
static struct {
    int status;
    const char *twin;
    const char *option;
    unsigned calls;
    const char *seen_name;
    const VmafFeatureDictionary *seen_opts;
    bool saw_pic_1080p;
} twin_fake;

static void script_twin(int status, const char *twin, const char *option)
{
    memset(&twin_fake, 0, sizeof(twin_fake));
    twin_fake.status = status;
    twin_fake.twin = twin;
    twin_fake.option = option;
}

int vmaf_feature_backend_twin(VmafContext *vmaf, const char *feature_name,
                              const VmafFeatureDictionary *opts_dict,
                              const VmafPictureConfiguration *pic_cfg, const char **twin_name,
                              const char **unsupported_option)
{
    (void)vmaf;
    twin_fake.calls++;
    twin_fake.seen_name = feature_name;
    twin_fake.seen_opts = opts_dict;
    twin_fake.saw_pic_1080p = pic_cfg == &pic_1080p;
    /* Same contract as the real function: no result pointer, no lookup. */
    if (!twin_name)
        return -EINVAL;
    *twin_name = twin_fake.twin;
    if (unsupported_option)
        *unsupported_option = twin_fake.option;
    return twin_fake.status;
}

static struct CliFeatureChoice choose(const char *backend, const char *feature, char *warn,
                                      size_t sz)
{
    return cli_choose_feature_extractor(NULL, backend, feature, NULL, &pic_1080p, warn, sz);
}

static char *test_twin_is_chosen_without_warning(void)
{
    char warn[256] = "stale";
    script_twin(0, "ciede_sycl", NULL);
    const struct CliFeatureChoice c = choose("sycl", "ciede", warn, sizeof(warn));
    mu_assert("twin not chosen", !strcmp(c.extractor, "ciede_sycl"));
    mu_assert("twin name not reported", c.twin_name && !strcmp(c.twin_name, "ciede_sycl"));
    mu_assert("status not 0", c.status == 0);
    mu_assert("warning printed for a chosen twin", warn[0] == '\0');
    mu_assert("lookup not asked exactly once", twin_fake.calls == 1U);
    mu_assert("lookup got a different name", !strcmp(twin_fake.seen_name, "ciede"));
    mu_assert("lookup got a different geometry", twin_fake.saw_pic_1080p);
    return NULL;
}

static char *test_no_twin_keeps_cpu_and_warns(void)
{
    char warn[256];
    script_twin(-ENOENT, NULL, NULL);
    const struct CliFeatureChoice c = choose("sycl", "brisque", warn, sizeof(warn));
    mu_assert("CPU extractor not kept", !strcmp(c.extractor, "brisque"));
    mu_assert("status lost", c.status == -ENOENT);
    mu_assert("warning does not name the feature", strstr(warn, "--feature brisque") != NULL);
    mu_assert("warning does not name the backend", strstr(warn, "sycl backend") != NULL);
    mu_assert("warning does not give the reason", strstr(warn, "no twin") != NULL);
    mu_assert("warning does not say where it runs", strstr(warn, "on the CPU\n") != NULL);
    return NULL;
}

static char *test_unhonoured_option_keeps_cpu_and_names_option(void)
{
    char warn[256];
    script_twin(-ENOTSUP, "float_ssim_sycl", "enable_lcs");
    const struct CliFeatureChoice c = choose("sycl", "float_ssim", warn, sizeof(warn));
    mu_assert("CPU extractor not kept", !strcmp(c.extractor, "float_ssim"));
    mu_assert("twin not named", strstr(warn, "float_ssim_sycl") != NULL);
    mu_assert("option not named", strstr(warn, "option 'enable_lcs'") != NULL);
    return NULL;
}

static char *test_unsupported_geometry_keeps_cpu_and_names_geometry(void)
{
    char warn[256];
    script_twin(-ENOTSUP, "float_ssim_sycl", NULL);
    const struct CliFeatureChoice c = choose("sycl", "float_ssim", warn, sizeof(warn));
    mu_assert("CPU extractor not kept", !strcmp(c.extractor, "float_ssim"));
    mu_assert("geometry not named", strstr(warn, "1920x1080 8-bit") != NULL);
    return NULL;
}

static char *test_disabled_backend_keeps_cpu_and_warns(void)
{
    char warn[256];
    script_twin(-ENODEV, NULL, NULL);
    const struct CliFeatureChoice c = choose("cuda", "ciede", warn, sizeof(warn));
    mu_assert("CPU extractor not kept", !strcmp(c.extractor, "ciede"));
    mu_assert("gpumask reason missing", strstr(warn, "--gpumask") != NULL);
    return NULL;
}

static char *test_other_error_keeps_cpu_and_reports_it(void)
{
    char warn[256];
    script_twin(-ENOMEM, "ciede_sycl", NULL);
    const struct CliFeatureChoice c = choose("sycl", "ciede", warn, sizeof(warn));
    mu_assert("twin chosen despite an error", !strcmp(c.extractor, "ciede"));
    mu_assert("error not reported", strstr(warn, "error -12") != NULL);
    return NULL;
}

/* A twin name (`ciede_sycl`), an unknown name or a bad option value: libvmaf
 * answers -EINVAL and the name is registered as given, which either works or
 * reports the error itself. */
static char *test_invalid_request_keeps_name_silently(void)
{
    char warn[256];
    script_twin(-EINVAL, NULL, NULL);
    const struct CliFeatureChoice c = choose("sycl", "ciede_sycl", warn, sizeof(warn));
    mu_assert("name not kept as given", !strcmp(c.extractor, "ciede_sycl"));
    mu_assert("warning printed for an invalid request", warn[0] == '\0');
    return NULL;
}

static char *test_cpu_auto_and_unset_backend_skip_lookup(void)
{
    const char *const backends[] = {"cpu", "auto", NULL};
    for (unsigned i = 0; i < 3U; i++) {
        char warn[64] = "stale";
        script_twin(0, "ciede_sycl", NULL);
        const struct CliFeatureChoice c = choose(backends[i], "ciede", warn, sizeof(warn));
        mu_assert("name changed without a device backend", !strcmp(c.extractor, "ciede"));
        mu_assert("libvmaf asked without a device backend", twin_fake.calls == 0U);
        mu_assert("warning without a device backend", warn[0] == '\0');
    }
    mu_assert("sycl is a device backend", cli_backend_is_device("sycl"));
    mu_assert("metal is a device backend", cli_backend_is_device("metal"));
    return NULL;
}

static char *test_warning_buffer_boundaries(void)
{
    char one[1] = {'x'};
    script_twin(-ENOENT, NULL, NULL);
    struct CliFeatureChoice c = choose("sycl", "brisque", one, sizeof(one));
    mu_assert("one-byte buffer not terminated", one[0] == '\0');
    mu_assert("one-byte buffer changed the choice", !strcmp(c.extractor, "brisque"));

    char small[16];
    memset(small, 'x', sizeof(small));
    c = choose("sycl", "brisque", small, sizeof(small));
    mu_assert("truncated warning not terminated", small[sizeof(small) - 1U] == '\0');
    mu_assert("truncated warning lost its prefix", !strncmp(small, "vmaf: warning:", 14));
    mu_assert("small buffer changed the choice", !strcmp(c.extractor, "brisque"));

    c = choose("sycl", "brisque", NULL, 0);
    mu_assert("NULL buffer changed the choice", !strcmp(c.extractor, "brisque"));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_twin_is_chosen_without_warning),
        MU_TEST(test_no_twin_keeps_cpu_and_warns),
        MU_TEST(test_unhonoured_option_keeps_cpu_and_names_option),
        MU_TEST(test_unsupported_geometry_keeps_cpu_and_names_geometry),
        MU_TEST(test_disabled_backend_keeps_cpu_and_warns),
        MU_TEST(test_other_error_keeps_cpu_and_reports_it),
        MU_TEST(test_invalid_request_keeps_name_silently),
        MU_TEST(test_cpu_auto_and_unset_backend_skip_lookup),
        MU_TEST(test_warning_buffer_boundaries),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
