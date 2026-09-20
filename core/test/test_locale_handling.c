/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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

#include "test.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "config.h"
#include "thread_locale.h"
#include "feature/feature_collector.h"
#include "output.h"
#include "read_json_model.h"

typedef struct TestLocaleState {
#ifdef _WIN32
    int previous_thread_locale_mode;
    char previous_locale[256];
#else
    locale_t locale;
    locale_t previous_locale;
#endif
} TestLocaleState;

static int test_locale_activate(TestLocaleState *state, const char *primary, const char *fallback)
{
#ifdef _WIN32
    state->previous_thread_locale_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    const char *const previous = setlocale(LC_ALL, VMAF_NULLPTR);
    if (!previous)
        return 0;
    (void)snprintf(state->previous_locale, sizeof(state->previous_locale), "%s", previous);
    if (!setlocale(LC_ALL, primary) && !setlocale(LC_ALL, fallback)) {
        (void)setlocale(LC_ALL, state->previous_locale);
        (void)_configthreadlocale(state->previous_thread_locale_mode);
        return 0;
    }
#else
    state->locale = newlocale(LC_ALL_MASK, primary, VMAF_NULLPTR);
    if (!state->locale)
        state->locale = newlocale(LC_ALL_MASK, fallback, VMAF_NULLPTR);
    if (!state->locale)
        return 0;
    state->previous_locale = uselocale(state->locale);
    if (!state->previous_locale) {
        freelocale(state->locale);
        state->locale = VMAF_NULLPTR;
        return 0;
    }
#endif
    return 1;
}

static void test_locale_restore(TestLocaleState *state)
{
#ifdef _WIN32
    (void)setlocale(LC_ALL, state->previous_locale);
    (void)_configthreadlocale(state->previous_thread_locale_mode);
#else
    (void)uselocale(state->previous_locale);
    freelocale(state->locale);
#endif
}

static char *read_output(FILE *stream, char *output, size_t capacity)
{
    if (fseek(stream, 0L, SEEK_SET) != 0)
        return "failed to seek output stream";
    const size_t bytes_read = fread(output, 1, capacity - 1u, stream);
    if (ferror(stream))
        return "failed to read output stream";
    output[bytes_read] = '\0';
    return VMAF_NULLPTR;
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

static char *verify_comma_locale_active(const char *message)
{
    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert(message, strchr(buffer, ',') != VMAF_NULLPTR);
    return VMAF_NULLPTR;
}

static char *verify_xml_output(const char *output)
{
    mu_assert("XML output should contain period decimals", contains_period_decimals(output));
    mu_assert("XML output should not contain 12,345", strstr(output, "12,345") == VMAF_NULLPTR);
    mu_assert("XML output should contain 12.3", strstr(output, "12.3") != VMAF_NULLPTR);
    return verify_comma_locale_active("French locale should still be active");
}

static char *verify_json_output(const char *output)
{
    mu_assert("JSON output should contain period decimals", contains_period_decimals(output));
    mu_assert("JSON output should not contain 98,765", strstr(output, "98,765") == VMAF_NULLPTR);
    return verify_comma_locale_active("Italian locale should still be active");
}

static char *test_locale_abstraction_basic(void)
{
    TestLocaleState locale;
    if (!test_locale_activate(&locale, "es_ES.UTF-8", "es_ES.utf8")) {
        (void)fprintf(stderr, "Skipping test: Spanish locale not available\n");
        return VMAF_NULLPTR;
    }

    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should use comma", strchr(buffer, ',') != VMAF_NULLPTR);

    VmafThreadLocaleState *state = vmaf_thread_locale_push_c();
    mu_assert("vmaf_thread_locale_push_c should not return NULL", state != VMAF_NULLPTR);

    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("C locale should use period", strchr(buffer, '.') != VMAF_NULLPTR);
    mu_assert("C locale should not use comma", strchr(buffer, ',') == VMAF_NULLPTR);

    vmaf_thread_locale_pop(state);

    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should be restored", strchr(buffer, ',') != VMAF_NULLPTR);

    test_locale_restore(&locale);

    return VMAF_NULLPTR;
}

static char *test_output_xml_with_comma_locale(void)
{
    TestLocaleState locale;
    if (!test_locale_activate(&locale, "fr_FR.UTF-8", "fr_FR.utf8")) {
        (void)fprintf(stderr, "Skipping test: French locale not available\n");
        return VMAF_NULLPTR;
    }

    int err;
    VmafContext *vmaf = VMAF_NULLPTR;
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

    FILE *tmpf = tmpfile();
    mu_assert("tmpfile creation failed", tmpf != VMAF_NULLPTR);

    err = vmaf_write_output_xml(vmaf, fc, tmpf, 1, 1920, 1080, 24.0, 1, VMAF_NULLPTR);
    mu_assert("vmaf_write_output_xml failed", !err);

    char output[4096];
    char *const read_error = read_output(tmpf, output, sizeof(output));
    (void)fclose(tmpf);
    mu_assert(read_error, read_error == VMAF_NULLPTR);

    char *const verification_error = verify_xml_output(output);
    mu_assert(verification_error, verification_error == VMAF_NULLPTR);

    vmaf_feature_collector_destroy(fc);
    vmaf_close(vmaf);
    test_locale_restore(&locale);

    return VMAF_NULLPTR;
}

static char *test_output_json_with_comma_locale(void)
{
    TestLocaleState locale;
    if (!test_locale_activate(&locale, "it_IT.UTF-8", "it_IT.utf8")) {
        (void)fprintf(stderr, "Skipping test: Italian locale not available\n");
        return VMAF_NULLPTR;
    }

    int err;
    VmafContext *vmaf = VMAF_NULLPTR;
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

    FILE *tmpf = tmpfile();
    mu_assert("tmpfile creation failed", tmpf != VMAF_NULLPTR);

    err = vmaf_write_output_json(vmaf, fc, tmpf, 1, 24.0, 1, VMAF_NULLPTR);
    mu_assert("vmaf_write_output_json failed", !err);

    char output[4096];
    char *const read_error = read_output(tmpf, output, sizeof(output));
    (void)fclose(tmpf);
    mu_assert(read_error, read_error == VMAF_NULLPTR);

    char *const verification_error = verify_json_output(output);
    mu_assert(verification_error, verification_error == VMAF_NULLPTR);

    vmaf_feature_collector_destroy(fc);
    vmaf_close(vmaf);
    test_locale_restore(&locale);

    return VMAF_NULLPTR;
}

static char *test_output_csv_with_comma_locale(void)
{
    TestLocaleState locale;
    if (!test_locale_activate(&locale, "es_ES.UTF-8", "es_ES.utf8")) {
        (void)fprintf(stderr, "Skipping test: Spanish locale not available\n");
        return VMAF_NULLPTR;
    }

    int err;
    VmafFeatureCollector *fc;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("vmaf_feature_collector_init failed", !err);

    err = vmaf_feature_collector_append(fc, "metric1", 45.678, 0);
    mu_assert("vmaf_feature_collector_append failed", !err);

    FILE *tmpf = tmpfile();
    mu_assert("tmpfile creation failed", tmpf != VMAF_NULLPTR);

    err = vmaf_write_output_csv(fc, tmpf, 1, VMAF_NULLPTR);
    mu_assert("vmaf_write_output_csv failed", !err);

    char output[4096];
    char *const read_error = read_output(tmpf, output, sizeof(output));
    (void)fclose(tmpf);
    mu_assert(read_error, read_error == VMAF_NULLPTR);

    mu_assert("CSV output should contain period decimals", contains_period_decimals(output));

    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should still be active", strchr(buffer, ',') != VMAF_NULLPTR);

    vmaf_feature_collector_destroy(fc);
    test_locale_restore(&locale);

    return VMAF_NULLPTR;
}

static char *test_model_parse_with_comma_locale(void)
{
    TestLocaleState locale;
    if (!test_locale_activate(&locale, "es_ES.UTF-8", "es_ES.utf8")) {
        (void)fprintf(stderr, "Skipping test: Spanish locale not available\n");
        return VMAF_NULLPTR;
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

    // Verify Spanish locale active
    char buffer[100];
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should be active", strchr(buffer, ',') != VMAF_NULLPTR);

    VmafModel *model = VMAF_NULLPTR;
    VmafModelConfig cfg = {
        .name = "test_model",
        .flags = VMAF_MODEL_FLAGS_DEFAULT,
    };

    int err = vmaf_read_json_model_from_buffer(&model, &cfg, model_json, strlen(model_json));
    mu_assert("vmaf_read_json_model_from_buffer should succeed", !err);
    mu_assert("model should not be NULL", model != VMAF_NULLPTR);

    // Verify values parsed correctly (with periods, not commas)
    mu_assert("slope should be 1.5", fabs(model->slope - 1.5) < 0.001);
    mu_assert("intercept should be 0.5", fabs(model->intercept - 0.5) < 0.001);

    // Verify Spanish locale still active
    (void)snprintf(buffer, sizeof(buffer), "%.2f", 3.14);
    mu_assert("Spanish locale should still be active", strchr(buffer, ',') != VMAF_NULLPTR);

    vmaf_model_destroy(model);
    test_locale_restore(&locale);

    return VMAF_NULLPTR;
}

#ifdef HAVE_USELOCALE
static char *test_uselocale_available(void)
{
    VmafThreadLocaleState *state = vmaf_thread_locale_push_c();
    mu_assert("HAVE_USELOCALE: push should succeed", state != VMAF_NULLPTR);
    vmaf_thread_locale_pop(state);
    return VMAF_NULLPTR;
}
#elif defined(_WIN32)
static char *test_windows_locale_handling(void)
{
    VmafThreadLocaleState *state = vmaf_thread_locale_push_c();
    mu_assert("Windows: push should succeed", state != VMAF_NULLPTR);
    vmaf_thread_locale_pop(state);
    return VMAF_NULLPTR;
}
#endif

char *run_tests(void)
{
    mu_run_test(test_locale_abstraction_basic);
    mu_run_test(test_output_xml_with_comma_locale);
    mu_run_test(test_output_json_with_comma_locale);
    mu_run_test(test_output_csv_with_comma_locale);
    mu_run_test(test_model_parse_with_comma_locale);

#ifdef HAVE_USELOCALE
    mu_run_test(test_uselocale_available);
#elif defined(_WIN32)
    mu_run_test(test_windows_locale_handling);
#endif

    return VMAF_NULLPTR;
}
