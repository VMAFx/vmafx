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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* ADR-0729 Wave 3: feature_name.c renamed to feature_name.cpp; update unity include. */
#include "feature/feature_name.cpp"

/* Test fixture for `vmaf_feature_name_from_provided_features()`: walks a
 * representative `VmafOption` table covering every option type (bool,
 * double, int, alias) and asserts that the generated feature-name string
 * encodes only options whose default value is overridden, in alias order.
 * lgtm[cpp/poorly-documented-function] — unit-test scaffolding; the body
 * is a sequence of `mu_assert` calls whose intent is conveyed by the
 * assertion strings themselves. CodeQL's path-ignore on `libvmaf/test`
 * (see `.github/codeql-config.yml`) suppresses this rule for new scans;
 * this comment carries the existing alert through to its next refresh. */
static mu_message_t test_feature_name_from_options()
{
    typedef struct TestState {
        bool opt_bool;
        double opt_double;
        int opt_int;
        bool opt_bool2;
    } TestState;

#define opt_bool_default false
#define opt_double_default 3.14
#define opt_int_default 200

    /* ADR-0729 Wave 3: C99 nested designators (.default_val.b) are a GCC
     * extension not permitted in standard C++. Replace with explicit union
     * member construction using C++20 designated initialiser syntax. */
    static VmafOption options[] = {{
                                       .name = "opt_bool",
                                       .offset = offsetof(TestState, opt_bool),
                                       .type = VMAF_OPT_TYPE_BOOL,
                                       .default_val = {.b = opt_bool_default},
                                       .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                   },
                                   {
                                       .name = "opt_double",
                                       .alias = "opt_double_alias",
                                       .offset = offsetof(TestState, opt_double),
                                       .type = VMAF_OPT_TYPE_DOUBLE,
                                       .default_val = {.d = opt_double_default},
                                       .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                   },
                                   {
                                       .name = "opt_int",
                                       .alias = "opt_int_alias",
                                       .offset = offsetof(TestState, opt_int),
                                       .type = VMAF_OPT_TYPE_INT,
                                       .default_val = {.i = opt_int_default},
                                       .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                   },
                                   {
                                       .name = "opt_bool2",
                                       .offset = offsetof(TestState, opt_bool),
                                       .type = VMAF_OPT_TYPE_BOOL,
                                       .default_val = {.b = opt_bool_default},
                                       .flags = 0,
                                   },
                                   {}};

    TestState s1 = {
        .opt_bool = opt_bool_default,
        .opt_double = opt_double_default,
        .opt_int = opt_int_default,
        .opt_bool2 = opt_bool_default,
    };

    char *feature_name1 = vmaf_feature_name_from_options("feature_name", options, &s1);

    mu_assert("when all options are default, feature_name should not change",
              !strcmp(feature_name1, "feature_name"));

    free(feature_name1);

    TestState s2 = {
        .opt_bool = !opt_bool_default,
        .opt_double = opt_double_default,
        .opt_int = opt_int_default,
        .opt_bool2 = opt_bool_default,
    };

    char *feature_name2 = vmaf_feature_name_from_options("feature_name", options, &s2);

    mu_assert("when opt_bool has a non-default value, "
              "feature_name should have a non-aliased opt_bool suffix",
              !strcmp(feature_name2, "feature_name_opt_bool"));

    free(feature_name2);

    TestState s3 = {
        .opt_bool = opt_bool_default,
        .opt_double = opt_double_default + 1,
        .opt_int = opt_int_default,
        .opt_bool2 = opt_bool_default,
    };

    char *feature_name3 = vmaf_feature_name_from_options("feature_name", options, &s3);

    mu_assert("when opt_double has a non-default value, "
              "feature_name should have a aliased opt_double_alias suffix",
              !strcmp(feature_name3, "feature_name_opt_double_alias_4.14"));

    free(feature_name3);

    TestState s4 = {
        .opt_bool = !opt_bool_default,
        .opt_double = opt_double_default + 1,
        .opt_int = opt_int_default + 1,
        .opt_bool2 = !opt_bool_default,
    };

    char *feature_name4 = vmaf_feature_name_from_options("feature_name", options, &s4);

    mu_assert(
        "when all opts have a non-default value, "
        "feature_name should have a suffix with aliases and values. "
        "opt_bool2 should not parameterize since its flags are unset.",
        !strcmp(feature_name4, "feature_name_opt_bool_opt_double_alias_4.14_opt_int_alias_201"));

    free(feature_name4);

    TestState s5 = s4;

    char *feature_name5 = vmaf_feature_name_from_options("feature_name", options, &s5);

    mu_assert(
        "feature_name should have a suffix with aliases and values, "
        "ordering should not follow the ordering of variadac params,"
        "rather it should follow the order of options",
        !strcmp(feature_name5, "feature_name_opt_bool_opt_double_alias_4.14_opt_int_alias_201"));

    free(feature_name5);

    return NULL;
}

/* Ported from the pre-conversion C twin core/test/test_feature.c (ADR-1153): these
 * seven assertions covered NULL handling, STRING options and dict allocation and
 * had no equivalent here, so the twin could not be deleted without them. */

static mu_message_t test_feature_name_null_inputs()
{
    char *out = vmaf_feature_name_from_options(NULL, NULL, NULL);
    mu_assert("name=NULL must return NULL", out == NULL);

    out = vmaf_feature_name_from_options("bare", NULL, NULL);
    mu_assert("opts=NULL must produce the unadorned name", !strcmp(out, "bare"));
    free(out);

    static VmafOption opts[] = {
        {.name = "dummy", .type = VMAF_OPT_TYPE_INT, .flags = VMAF_OPT_FLAG_FEATURE_PARAM}, {0}};

    out = vmaf_feature_name_from_options("bare2", opts, NULL);
    mu_assert("obj=NULL must short-circuit to unadorned name", !strcmp(out, "bare2"));
    free(out);

    return NULL;
}

static mu_message_t test_feature_name_string_option()
{
    typedef struct {
        char *mode;
    } StringState;

    static char default_mode[] = "auto";
    static VmafOption opts[] = {{
                                    .name = "mode",
                                    .offset = offsetof(StringState, mode),
                                    .type = VMAF_OPT_TYPE_STRING,
                                    .default_val = {.s = default_mode},
                                    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                },
                                {0}};

    StringState s_default = {.mode = default_mode};
    char *out = vmaf_feature_name_from_options("fname", opts, &s_default);
    mu_assert("default STRING option must not appear in feature_name", !strcmp(out, "fname"));
    free(out);

    static char custom_mode[] = "fast";
    StringState s_custom = {.mode = custom_mode};
    out = vmaf_feature_name_from_options("fname", opts, &s_custom);
    mu_assert("non-default STRING option must be appended to feature_name",
              !strcmp(out, "fname_mode_fast"));
    free(out);

    return NULL;
}

static mu_message_t test_feature_name_dict_from_provided_features()
{
    static const char *provided[] = {"a", "b", NULL};

    typedef struct {
        int dummy;
    } DummyState;
    static VmafOption opts[] = {{0}};
    DummyState s = {0};

    VmafDictionary *dict = vmaf_feature_name_dict_from_provided_features(provided, opts, &s);
    mu_assert("dict must be allocated when provided_features is non-empty", dict != NULL);
    if (dict)
        vmaf_dictionary_free(&dict);

    static const char *empty[] = {NULL};
    dict = vmaf_feature_name_dict_from_provided_features(empty, opts, &s);
    mu_assert("dict must be NULL when provided_features is empty", dict == NULL);

    return NULL;
}

mu_message_t run_tests()
{
    mu_run_test(test_feature_name_from_options);
    mu_run_test(test_feature_name_null_inputs);
    mu_run_test(test_feature_name_string_option);
    mu_run_test(test_feature_name_dict_from_provided_features);
    return NULL;
}
