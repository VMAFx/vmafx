/**
 *
 *  Copyright 2026 Lusoris
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

/*
 * Regression tests for the vendored libsvm text-format model parser
 * hardening (SAN-MODEL-MALLOC-OOB; see ADR-0889).
 *
 * The parser must reject malformed model files BEFORE the size derived
 * from a header field flows into a `Malloc(...)` call or before a
 * support-vector buffer is dereferenced at a yet-unallocated index.
 *
 * Each fixture below is a crafted text-format SVM model whose header
 * triggers a specific oob / read-before-write defect that the original
 * libsvm 3.24 parser had. `svm_parse_model_from_buffer` must return
 * `NULL` on every fixture rather than crash, returning a partially
 * constructed `svm_model`, or producing a heap-corruption signal under
 * ASan.
 */

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "svm.h"

/*
 * Quietens libsvm's stderr `info()` print during parser-failure tests
 * so the test runner output stays diff-clean.
 */
static void silence_svm_log(const char *s)
{
    (void)s;
}

static char *test_reject_oversized_nr_class(void)
{
    /* nr_class above VMAF_SVM_MAX_AXIS_COUNT (1 << 24) must be rejected
     * before it flows into a per-class Malloc. */
    static const char model[] = "svm_type epsilon_svr\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "nr_class 33554433\n"
                                "total_sv 100\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject nr_class > VMAF_SVM_MAX_AXIS_COUNT", m == NULL);
    return NULL;
}

static char *test_reject_oversized_total_sv(void)
{
    /* total_sv above VMAF_SVM_MAX_AXIS_COUNT must be rejected before it
     * flows into the SV-coefficient allocation. */
    static const char model[] = "svm_type epsilon_svr\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "nr_class 2\n"
                                "total_sv 33554433\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject total_sv > VMAF_SVM_MAX_AXIS_COUNT", m == NULL);
    return NULL;
}

static char *test_reject_missing_nr_class_before_rho(void)
{
    /* `rho` appearing before `nr_class` would (pre-ADR-0889) flow a 0
     * permutation-count into Malloc(double, 0) and leave model->rho
     * pointing at an implementation-defined zero-size buffer. */
    static const char model[] = "svm_type epsilon_svr\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "rho 0\n"
                                "nr_class 2\n"
                                "total_sv 1\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject rho before nr_class", m == NULL);
    return NULL;
}

static char *test_reject_missing_nr_class_before_label(void)
{
    static const char model[] = "svm_type c_svc\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "label 0 1\n"
                                "nr_class 2\n"
                                "total_sv 1\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject label before nr_class", m == NULL);
    return NULL;
}

static char *test_reject_missing_nr_class_before_probA(void)
{
    static const char model[] = "svm_type c_svc\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "probA 0\n"
                                "probB 0\n"
                                "nr_class 2\n"
                                "total_sv 1\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject probA before nr_class", m == NULL);
    return NULL;
}

static char *test_reject_missing_nr_class_before_nr_sv(void)
{
    static const char model[] = "svm_type c_svc\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "nr_sv 1 0\n"
                                "nr_class 2\n"
                                "total_sv 1\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject nr_sv before nr_class", m == NULL);
    return NULL;
}

static char *test_reject_missing_nr_class_at_sv_parse(void)
{
    /* Header omits nr_class entirely; parse_support_vectors must reject
     * before computing `Malloc(double *, model->nr_class - 1)`. */
    static const char model[] = "svm_type epsilon_svr\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "total_sv 1\n"
                                "SV\n"
                                "1.0 1:0.5\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject missing nr_class at SV parse", m == NULL);
    return NULL;
}

static char *test_reject_empty_sv_section(void)
{
    /* total_sv claims one SV but the SV section is empty; the
     * sv_buffer.empty() guard must catch this rather than feed 0
     * to Malloc + memcpy. */
    static const char model[] = "svm_type epsilon_svr\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "nr_class 2\n"
                                "total_sv 0\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject zero total_sv", m == NULL);
    return NULL;
}

static char *test_reject_unknown_svm_type(void)
{
    static const char model[] = "svm_type bogus_type\n"
                                "kernel_type rbf\n"
                                "gamma 0.05\n"
                                "nr_class 2\n"
                                "total_sv 1\n"
                                "rho 0\n"
                                "SV\n";
    svm_set_print_string_function(&silence_svm_log);
    struct svm_model *m = svm_parse_model_from_buffer(model, (unsigned int)(sizeof(model) - 1));
    mu_assert("parser must reject unknown svm_type", m == NULL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_reject_oversized_nr_class);
    mu_run_test(test_reject_oversized_total_sv);
    mu_run_test(test_reject_missing_nr_class_before_rho);
    mu_run_test(test_reject_missing_nr_class_before_label);
    mu_run_test(test_reject_missing_nr_class_before_probA);
    mu_run_test(test_reject_missing_nr_class_before_nr_sv);
    mu_run_test(test_reject_missing_nr_class_at_sv_parse);
    mu_run_test(test_reject_empty_sv_section);
    mu_run_test(test_reject_unknown_svm_type);
    return NULL;
}
