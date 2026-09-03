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

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "test.h"

/* ADR-0729 Wave 3: feature_name.c renamed to feature_name.cpp; the test drives
 * translation-unit-local helpers, so the implementation is unity-included rather
 * than linked. The .cpp extension is deliberate and load-bearing here. */
// NOLINTNEXTLINE(bugprone-suspicious-include) — ADR-0729 unity include, see above
#include "feature/feature_name.cpp"

/* Fixtures for `vmaf_feature_name_from_options()` and
 * `vmaf_feature_name_dict_from_provided_features()`. The option table below is
 * representative: it covers every option type (bool, double, int, string,
 * alias) so the generated feature-name string can be asserted to encode only
 * options whose default value is overridden, in alias order.
 * lgtm[cpp/poorly-documented-function] — unit-test scaffolding; each body is a
 * sequence of `mu_assert` calls whose intent is conveyed by the assertion
 * strings themselves. CodeQL's path-ignore on `libvmaf/test`
 * (see `.github/codeql-config.yml`) suppresses this rule for new scans;
 * this comment carries the existing alert through to its next refresh. */

namespace
{

constexpr bool kOptBoolDefault = false;
constexpr double kOptDoubleDefault = 3.14;
constexpr int kOptIntDefault = 200;

struct TestState {
    bool opt_bool;
    double opt_double;
    int opt_int;
    bool opt_bool2;
};

/* ADR-0729 Wave 3: C99 nested designators (.default_val.b) are a GCC extension
 * not permitted in standard C++. Replace with explicit union member
 * construction using C++20 designated initialiser syntax. */
VmafOption g_options[] = {{
                              .name = "opt_bool",
                              .offset = offsetof(TestState, opt_bool),
                              .type = VMAF_OPT_TYPE_BOOL,
                              .default_val = {.b = kOptBoolDefault},
                              .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                          },
                          {
                              .name = "opt_double",
                              .alias = "opt_double_alias",
                              .offset = offsetof(TestState, opt_double),
                              .type = VMAF_OPT_TYPE_DOUBLE,
                              .default_val = {.d = kOptDoubleDefault},
                              .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                          },
                          {
                              .name = "opt_int",
                              .alias = "opt_int_alias",
                              .offset = offsetof(TestState, opt_int),
                              .type = VMAF_OPT_TYPE_INT,
                              .default_val = {.i = kOptIntDefault},
                              .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                          },
                          {
                              .name = "opt_bool2",
                              .offset = offsetof(TestState, opt_bool),
                              .type = VMAF_OPT_TYPE_BOOL,
                              .default_val = {.b = kOptBoolDefault},
                              .flags = 0,
                          },
                          {}};

/* Every override case shares the same baseline: only the fields a case names
 * differ from the option table's defaults. */
constexpr TestState kAllDefaults = {
    .opt_bool = kOptBoolDefault,
    .opt_double = kOptDoubleDefault,
    .opt_int = kOptIntDefault,
    .opt_bool2 = kOptBoolDefault,
};

mu_message_t test_feature_name_all_defaults()
{
    TestState s = kAllDefaults;
    char *feature_name = vmaf_feature_name_from_options("feature_name", g_options, &s);
    const bool ok = feature_name && !strcmp(feature_name, "feature_name");
    free(feature_name);

    mu_assert("when all options are default, feature_name should not change", ok);
    return nullptr;
}

mu_message_t test_feature_name_bool_override()
{
    TestState s = kAllDefaults;
    s.opt_bool = !kOptBoolDefault;

    char *feature_name = vmaf_feature_name_from_options("feature_name", g_options, &s);

    const bool ok = feature_name && !strcmp(feature_name, "feature_name_opt_bool");
    free(feature_name);

    mu_assert("when opt_bool has a non-default value, "
              "feature_name should have a non-aliased opt_bool suffix",
              ok);
    return nullptr;
}

mu_message_t test_feature_name_double_override()
{
    TestState s = kAllDefaults;
    s.opt_double = kOptDoubleDefault + 1;

    char *feature_name = vmaf_feature_name_from_options("feature_name", g_options, &s);

    const bool ok = feature_name && !strcmp(feature_name, "feature_name_opt_double_alias_4.14");
    free(feature_name);

    mu_assert("when opt_double has a non-default value, "
              "feature_name should have a aliased opt_double_alias suffix",
              ok);
    return nullptr;
}

mu_message_t test_feature_name_all_overridden()
{
    TestState s4 = {
        .opt_bool = !kOptBoolDefault,
        .opt_double = kOptDoubleDefault + 1,
        .opt_int = kOptIntDefault + 1,
        .opt_bool2 = !kOptBoolDefault,
    };

    char *feature_name4 = vmaf_feature_name_from_options("feature_name", g_options, &s4);

    const bool ok4 =
        feature_name4 &&
        !strcmp(feature_name4, "feature_name_opt_bool_opt_double_alias_4.14_opt_int_alias_201");
    free(feature_name4);

    mu_assert("when all opts have a non-default value, "
              "feature_name should have a suffix with aliases and values. "
              "opt_bool2 should not parameterize since its flags are unset.",
              ok4);

    TestState s5 = s4;

    char *feature_name5 = vmaf_feature_name_from_options("feature_name", g_options, &s5);

    const bool ok5 =
        feature_name5 &&
        !strcmp(feature_name5, "feature_name_opt_bool_opt_double_alias_4.14_opt_int_alias_201");
    free(feature_name5);

    mu_assert("feature_name should have a suffix with aliases and values, "
              "ordering should not follow the ordering of variadac params,"
              "rather it should follow the order of options",
              ok5);
    return nullptr;
}

/* Ported from the pre-conversion C twin core/test/test_feature.c (ADR-1153): these
 * seven assertions covered null handling, STRING options and dict allocation and
 * had no equivalent here, so the twin could not be deleted without them. */

mu_message_t test_feature_name_null_inputs()
{
    char *out = vmaf_feature_name_from_options(nullptr, nullptr, nullptr);
    mu_assert("name=nullptr must return nullptr", out == nullptr);

    out = vmaf_feature_name_from_options("bare", nullptr, nullptr);
    const bool bare_ok = out && !strcmp(out, "bare");
    free(out);
    mu_assert("opts=nullptr must produce the unadorned name", bare_ok);

    static VmafOption opts[] = {
        {.name = "dummy", .type = VMAF_OPT_TYPE_INT, .flags = VMAF_OPT_FLAG_FEATURE_PARAM}, {}};

    out = vmaf_feature_name_from_options("bare2", opts, nullptr);
    const bool bare2_ok = out && !strcmp(out, "bare2");
    free(out);
    mu_assert("obj=nullptr must short-circuit to unadorned name", bare2_ok);

    return nullptr;
}

mu_message_t test_feature_name_string_option()
{
    struct StringState {
        char *mode;
    };

    static char default_mode[] = "auto";
    static VmafOption opts[] = {{
                                    .name = "mode",
                                    .offset = offsetof(StringState, mode),
                                    .type = VMAF_OPT_TYPE_STRING,
                                    .default_val = {.s = default_mode},
                                    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                },
                                {}};

    StringState s_default = {.mode = default_mode};
    char *out = vmaf_feature_name_from_options("fname", opts, &s_default);
    const bool default_ok = out && !strcmp(out, "fname");
    free(out);
    mu_assert("default STRING option must not appear in feature_name", default_ok);

    static char custom_mode[] = "fast";
    StringState s_custom = {.mode = custom_mode};
    out = vmaf_feature_name_from_options("fname", opts, &s_custom);
    const bool custom_ok = out && !strcmp(out, "fname_mode_fast");
    free(out);
    mu_assert("non-default STRING option must be appended to feature_name", custom_ok);

    return nullptr;
}

mu_message_t test_feature_name_dict_from_provided_features()
{
    static const char *provided[] = {"a", "b", nullptr};

    struct DummyState {
        int dummy;
    };
    static VmafOption opts[] = {{}};
    DummyState s = {0};

    VmafDictionary *dict = vmaf_feature_name_dict_from_provided_features(provided, opts, &s);
    mu_assert("dict must be allocated when provided_features is non-empty", dict != nullptr);
    if (dict)
        vmaf_dictionary_free(&dict);

    static const char *empty[] = {nullptr};
    dict = vmaf_feature_name_dict_from_provided_features(empty, opts, &s);
    mu_assert("dict must be nullptr when provided_features is empty", dict == nullptr);

    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_run_test(test_feature_name_all_defaults);
    mu_run_test(test_feature_name_bool_override);
    mu_run_test(test_feature_name_double_override);
    mu_run_test(test_feature_name_all_overridden);
    mu_run_test(test_feature_name_null_inputs);
    mu_run_test(test_feature_name_string_option);
    mu_run_test(test_feature_name_dict_from_provided_features);
    return nullptr;
}
