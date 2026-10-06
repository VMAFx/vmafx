/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *
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

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mu_table.h"
#include "test.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "config.h"
#include "thread_locale.h"
#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/feature_name.h"
#include "opt.h"
#include "output.h"
#include "read_json_model.h"

/* NOLINTBEGIN(concurrency-mt-unsafe): Thread-local locale isolation test suite
 * exercises setlocale / uselocale intentionally to verify numeric parsing and
 * formatting across diverse locales (ADR-0141). */
/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

// Helper function to check if a locale is available
static int locale_available(const char *locale_name)
{
    char old_locale_buf[256];
    const char *old_locale = setlocale(LC_ALL, NULL);
    if (old_locale) {
        strncpy(old_locale_buf, old_locale, sizeof(old_locale_buf) - 1);
        old_locale_buf[sizeof(old_locale_buf) - 1] = '\0';
    } else {
        old_locale_buf[0] = '\0';
    }

    char *result = setlocale(LC_ALL, locale_name);
    int available = (result != NULL);

    if (old_locale_buf[0] != '\0') {
        (void)setlocale(LC_ALL, old_locale_buf);
    }

    return available;
}

// Helper function to verify a string contains period-formatted decimal
// numbers. Returns true iff at least one digit-period-digit triple
// appears (e.g. "12.345"). A digit-comma-digit triple is NOT
// interpreted as a leak signal here — CSV uses commas as structural
// field separators between integer cells (e.g. "0,3" between "Frame"
// column and first numeric column), and JSON uses trailing structural
// commas. If the thread-local C-locale applied correctly, the output
// will contain period decimals; if it did not, every numeric field
// would use commas and no period-decimal triple would appear, so the
// presence check is sufficient.
static int contains_period_decimals(const char *str)
{
    const char *p = str;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            char next = *(p + 1);
            char after = next ? *(p + 2) : '\0';
            if (next == '.' && after >= '0' && after <= '9') {
                return 1;
            }
        }
        p++;
    }
    return 0;
}

static const char *get_available_locale(const char *name1, const char *name2)
{
    if (locale_available(name1))
        return name1;
    if (locale_available(name2))
        return name2;
    return NULL;
}

static char *read_and_close_tmpfile(FILE *tmpf, char *buf, size_t buf_sz)
{
    int err = fseek(tmpf, 0, SEEK_SET);
    mu_assert("fseek failed", !err);
    size_t bytes_read = fread(buf, 1, buf_sz - 1, tmpf);
    buf[bytes_read] = '\0';
    err = fclose(tmpf);
    mu_assert("fclose failed", !err);
    return NULL;
}

static char *verify_xml_output_buffer(const char *output)
{
    mu_assert("XML output should contain period decimals", contains_period_decimals(output));
    mu_assert("XML output should not contain 12,345", strstr(output, "12,345") == NULL);
    mu_assert("XML output should contain 12.3", strstr(output, "12.3") != NULL);
    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("French locale should still be active", strchr(buffer, ',') != NULL);
    return NULL;
}

static char *verify_json_output_buffer(const char *output)
{
    mu_assert("JSON output should contain period decimals", contains_period_decimals(output));
    mu_assert("JSON output should not contain 98,765", strstr(output, "98,765") == NULL);
    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Italian locale should still be active", strchr(buffer, ',') != NULL);
    return NULL;
}

static char *verify_csv_output_buffer(const char *output)
{
    mu_assert("CSV output should contain period decimals", contains_period_decimals(output));
    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should still be active", strchr(buffer, ',') != NULL);
    return NULL;
}

static char *test_locale_abstraction_basic(void)
{
    // Test basic push/pop functionality
    const char *spanish = get_available_locale("es_ES.UTF-8", "es_ES.utf8");
    if (!spanish) {
        (void)fprintf(stderr, "Skipping test: Spanish locale not available\n");
        return NULL;
    }

    (void)setlocale(LC_ALL, spanish);

    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should use comma", strchr(buffer, ',') != NULL);

    VmafThreadLocaleState *state = vmaf_thread_locale_push_c();
    mu_assert("vmaf_thread_locale_push_c should not return NULL", state != NULL);

    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("C locale should use period", strchr(buffer, '.') != NULL);
    mu_assert("C locale should not use comma", strchr(buffer, ',') == NULL);

    vmaf_thread_locale_pop(state);

    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should be restored", strchr(buffer, ',') != NULL);

    (void)setlocale(LC_ALL, "C");

    return NULL;
}

static char *test_output_xml_with_comma_locale(void)
{
    const char *french = get_available_locale("fr_FR.UTF-8", "fr_FR.utf8");
    if (!french) {
        (void)fprintf(stderr, "Skipping test: French locale not available\n");
        return NULL;
    }

    int err;
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 0,
        .n_subsample = 1,
    };

    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    VmafFeatureCollector *fc;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("vmaf_feature_collector_init failed", !err);

    err = vmaf_feature_collector_append(fc, "test_feature", 12.345, 0);
    mu_assert("vmaf_feature_collector_append failed", !err);

    (void)setlocale(LC_ALL, french);

    FILE *tmpf = tmpfile();
    mu_assert("tmpfile creation failed", tmpf != NULL);

    err = vmaf_write_output_xml(vmaf, fc, tmpf, 1, 1920, 1080, 24.0, 1, NULL);
    mu_assert("vmaf_write_output_xml failed", !err);

    char output[4096];
    char *msg = read_and_close_tmpfile(tmpf, output, sizeof(output));
    if (msg)
        return msg;

    msg = verify_xml_output_buffer(output);
    if (msg)
        return msg;

    vmaf_feature_collector_destroy(fc);
    vmaf_close(vmaf);
    (void)setlocale(LC_ALL, "C");

    return NULL;
}

static char *test_output_json_with_comma_locale(void)
{
    const char *italian = get_available_locale("it_IT.UTF-8", "it_IT.utf8");
    if (!italian) {
        (void)fprintf(stderr, "Skipping test: Italian locale not available\n");
        return NULL;
    }

    int err;
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 0,
        .n_subsample = 1,
    };

    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    VmafFeatureCollector *fc;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("vmaf_feature_collector_init failed", !err);

    err = vmaf_feature_collector_append(fc, "test_feature", 98.765, 0);
    mu_assert("vmaf_feature_collector_append failed", !err);

    (void)setlocale(LC_ALL, italian);

    FILE *tmpf = tmpfile();
    mu_assert("tmpfile creation failed", tmpf != NULL);

    err = vmaf_write_output_json(vmaf, fc, tmpf, 1, 24.0, 1, NULL);
    mu_assert("vmaf_write_output_json failed", !err);

    char output[4096];
    char *msg = read_and_close_tmpfile(tmpf, output, sizeof(output));
    if (msg)
        return msg;

    msg = verify_json_output_buffer(output);
    if (msg)
        return msg;

    vmaf_feature_collector_destroy(fc);
    vmaf_close(vmaf);
    (void)setlocale(LC_ALL, "C");

    return NULL;
}

static char *test_output_csv_with_comma_locale(void)
{
    const char *spanish = get_available_locale("es_ES.UTF-8", "es_ES.utf8");
    if (!spanish) {
        (void)fprintf(stderr, "Skipping test: Spanish locale not available\n");
        return NULL;
    }

    int err;
    VmafFeatureCollector *fc;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("vmaf_feature_collector_init failed", !err);

    err = vmaf_feature_collector_append(fc, "metric1", 45.678, 0);
    mu_assert("vmaf_feature_collector_append failed", !err);

    (void)setlocale(LC_ALL, spanish);

    FILE *tmpf = tmpfile();
    mu_assert("tmpfile creation failed", tmpf != NULL);

    err = vmaf_write_output_csv(fc, tmpf, 1, NULL);
    mu_assert("vmaf_write_output_csv failed", !err);

    char output[4096];
    char *msg = read_and_close_tmpfile(tmpf, output, sizeof(output));
    if (msg)
        return msg;

    msg = verify_csv_output_buffer(output);
    if (msg)
        return msg;

    vmaf_feature_collector_destroy(fc);
    (void)setlocale(LC_ALL, "C");

    return NULL;
}

static char *test_model_parse_with_comma_locale(void)
{
    const char *spanish = get_available_locale("es_ES.UTF-8", "es_ES.utf8");
    if (!spanish) {
        (void)fprintf(stderr, "Skipping test: Spanish locale not available\n");
        return NULL;
    }

    // Simple JSON model with floating point values
    const char *model_json = "{"
                             "\"model_dict\":{"
                             "\"model_type\":\"LIBSVMNUSVR\","
                             "\"norm_type\":\"linear_rescale\","
                             "\"slopes\":[1.5,2.5],"
                             "\"intercepts\":[0.5,1.5],"
                             "\"score_clip\":[0.0,100.0],"
                             "\"feature_names\":[\"feature1\"]"
                             "}}";

    (void)setlocale(LC_ALL, spanish);

    // Verify Spanish locale active
    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should be active", strchr(buffer, ',') != NULL);

    VmafModel *model = NULL;
    VmafModelConfig cfg = {
        .name = "test_model",
        .flags = VMAF_MODEL_FLAGS_DEFAULT,
    };

    int err = vmaf_read_json_model_from_buffer(&model, &cfg, model_json, strlen(model_json));
    mu_assert("vmaf_read_json_model_from_buffer should succeed", !err);
    mu_assert("model should not be NULL", model != NULL);

    // Verify values parsed correctly (with periods, not commas)
    mu_assert("slope should be 1.5", fabs(model->slope - 1.5) < 0.001);
    mu_assert("intercept should be 0.5", fabs(model->intercept - 0.5) < 0.001);

    // Verify Spanish locale still active
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should still be active", strchr(buffer, ',') != NULL);

    vmaf_model_destroy(model);
    (void)setlocale(LC_ALL, "C");

    return NULL;
}

/* A decimal-comma locale, or NULL when the host has none. */
static const char *comma_locale(void)
{
    const char *name = get_available_locale("de_DE.UTF-8", "de_DE.utf8");
    return name ? name : get_available_locale("fr_FR.UTF-8", "fr_FR.utf8");
}

typedef struct {
    double d;
} DoubleTarget;

/* Feature options read "0.7" as 0.7 whatever the caller's numeric locale:
 * strtod() of a decimal-comma locale stops at the period, and the option
 * was refused (-EINVAL), so a model whose features take fractional options
 * (vmaf_v1.0.16_3d0h) could not be used.
 * T-OPTION-NUMBERS-CALLER-LOCALE-2026-10-06. */
static char *test_option_double_with_comma_locale(void)
{
    const char *locale = comma_locale();
    if (!locale) {
        (void)fprintf(stderr, "Skipping test: no decimal-comma locale available\n");
        return NULL;
    }
    const VmafOption opt = {.name = "x",
                            .offset = 0,
                            .type = VMAF_OPT_TYPE_DOUBLE,
                            .default_val.d = 1.0,
                            .min = 0.0,
                            .max = 10.0};
    DoubleTarget target = {0.0};
    (void)setlocale(LC_ALL, locale);
    const int err = vmaf_option_set(&opt, &target, "0.7");
    char buffer[16];
    (void)snprintf(buffer, sizeof(buffer), "%.1f", 0.5);
    (void)setlocale(LC_ALL, "C");
    mu_assert("the caller's locale stays active", strchr(buffer, ',') != NULL);
    mu_assert("0.7 is read as 0.7 under a decimal-comma locale", err == 0 && target.d == 0.7);
    return NULL;
}

/* The feature dictionary normalises "0.7" to "0.7", not to "0" (strtod()
 * stopped at the period) or "0,7" (%g of a decimal-comma locale). */
static char *test_dictionary_number_with_comma_locale(void)
{
    const char *locale = comma_locale();
    if (!locale) {
        (void)fprintf(stderr, "Skipping test: no decimal-comma locale available\n");
        return NULL;
    }
    VmafDictionary *dict = NULL;
    (void)setlocale(LC_ALL, locale);
    const int err = vmaf_dictionary_set(&dict, "nw", "0.02", VMAF_DICT_NORMALIZE_NUMERICAL_VALUES);
    (void)setlocale(LC_ALL, "C");
    const VmafDictionaryEntry *entry = err ? NULL : vmaf_dictionary_get(&dict, "nw", 0);
    const int same = entry && strcmp(entry->val, "0.02") == 0;
    (void)vmaf_dictionary_free(&dict);
    mu_assert("0.02 stays 0.02 under a decimal-comma locale", same);
    return NULL;
}

typedef struct {
    double strength;
} NamedTarget;

/* A fractional option names its feature with the period, as the models read
 * it, also under a decimal-comma locale ("x_strength_0.7", not "0,7"). */
static char *test_feature_name_with_comma_locale(void)
{
    const char *locale = comma_locale();
    if (!locale) {
        (void)fprintf(stderr, "Skipping test: no decimal-comma locale available\n");
        return NULL;
    }
    static const VmafOption opts[] = {
        {.name = "strength",
         .offset = 0,
         .type = VMAF_OPT_TYPE_DOUBLE,
         .default_val.d = 1.0,
         .min = 0.0,
         .max = 10.0,
         .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
        {0},
    };
    const NamedTarget target = {0.7};
    (void)setlocale(LC_ALL, locale);
    char *name = vmaf_feature_name_from_options("x", opts, &target);
    (void)setlocale(LC_ALL, "C");
    const int same = name && strcmp(name, "x_strength_0.7") == 0;
    free(name);
    mu_assert("the feature name carries 0.7 with a period", same);
    return NULL;
}

/* End to end: the features of the default model register under a
 * decimal-comma locale. */
static char *test_model_features_with_comma_locale(void)
{
    const char *locale = comma_locale();
    if (!locale) {
        (void)fprintf(stderr, "Skipping test: no decimal-comma locale available\n");
        return NULL;
    }
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafModel *model = NULL;
    VmafModelConfig model_cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    (void)setlocale(LC_ALL, locale);
    int err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_model_load(&model, &model_cfg, VMAF_DEFAULT_MODEL_VERSION);
    if (!err)
        err = vmaf_use_features_from_model(vmaf, model);
    (void)setlocale(LC_ALL, "C");
    (void)vmaf_close(vmaf);
    vmaf_model_destroy(model);
    mu_assert("the default model's features register under a decimal-comma locale", err == 0);
    return NULL;
}

#ifdef HAVE_USELOCALE
static char *test_uselocale_available(void)
{
    VmafThreadLocaleState *state = vmaf_thread_locale_push_c();
    mu_assert("HAVE_USELOCALE: push should succeed", state != NULL);
    vmaf_thread_locale_pop(state);
    return NULL;
}
#elif defined(_WIN32)
static char *test_windows_locale_handling(void)
{
    VmafThreadLocaleState *state = vmaf_thread_locale_push_c();
    mu_assert("Windows: push should succeed", state != NULL);
    vmaf_thread_locale_pop(state);
    return NULL;
}
#endif

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_locale_abstraction_basic),
        MU_TEST(test_output_xml_with_comma_locale),
        MU_TEST(test_output_json_with_comma_locale),
        MU_TEST(test_output_csv_with_comma_locale),
        MU_TEST(test_model_parse_with_comma_locale),
        MU_TEST(test_option_double_with_comma_locale),
        MU_TEST(test_dictionary_number_with_comma_locale),
        MU_TEST(test_feature_name_with_comma_locale),
        MU_TEST(test_model_features_with_comma_locale),
#ifdef HAVE_USELOCALE
        MU_TEST(test_uselocale_available),
#elif defined(_WIN32)
        MU_TEST(test_windows_locale_handling),
#endif
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
/* NOLINTEND(concurrency-mt-unsafe) */
