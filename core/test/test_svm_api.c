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
 * Coverage-targeted unit tests for the vendored libsvm runtime API
 * (svm_train + the inspector and predict family + svm_check_parameter
 * rejection branches + a save/load round-trip).
 *
 * This file is *complementary* to PR #381's test_svm_parser.c, which
 * focuses on the buffer-parser rejection paths added by ADR-0889. The
 * coverage gap left after PR #381 is the runtime side of svm.cpp —
 * train / predict / predict_values / predict_probability /
 * check_parameter / save / cross-validate. Those code paths account
 * for >70% of svm.cpp by line count and were at 9.6% coverage on
 * master before this file.
 *
 * The vendored libsvm 3.24 source is wrapped in a file-level
 * NOLINTBEGIN/NOLINTEND cordon and must not be modified semantically
 * (see core/src/AGENTS.md §10, ADR-0889). This test file is
 * observation-only — every assertion drives the existing public API
 * from svm.h.
 *
 * Tests use a tiny linearly-separable 2-class fixture so svm_train
 * converges in milliseconds while still touching the C_SVC training
 * loop, label assignment, and decision-function construction.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h> /* _close */
#else
#include <unistd.h>
#endif

#include "svm.h"
#include "test.h"

/* Silence libsvm's diagnostic output so the test log stays focused on
 * mu_assert results. */
static void silence_svm_log(const char *s)
{
    (void)s;
}

/* ------------------------------------------------------------------ */
/*  Fixture builders                                                   */
/* ------------------------------------------------------------------ */

/* Build a tiny 2-class linearly-separable dataset:
 *   class +1: points around (+2, +2), (+3, +1), (+2, +3), (+3, +3)
 *   class -1: points around (-2, -2), (-3, -1), (-2, -3), (-3, -3)
 *
 * The caller owns the returned arrays — release via free_problem().
 * Each sample's svm_node array is terminated with index = -1 per the
 * libsvm sparse format.
 */
struct binary_fixture {
    struct svm_problem prob;
    struct svm_node **nodes;
    double *y_storage;
    struct svm_node *node_storage;
};

static int build_binary_problem(struct binary_fixture *fx)
{
    const int n = 8;
    const int nz_per_row = 3; /* 2 features + sentinel */
    fx->prob.l = n;
    fx->y_storage = (double *)calloc((size_t)n, sizeof(double));
    fx->nodes = (struct svm_node **)calloc((size_t)n, sizeof(struct svm_node *));
    fx->node_storage =
        (struct svm_node *)calloc((size_t)n * (size_t)nz_per_row, sizeof(struct svm_node));
    if (!fx->y_storage || !fx->nodes || !fx->node_storage) {
        free(fx->y_storage);
        free((void *)fx->nodes);
        free(fx->node_storage);
        return -1;
    }
    const double xs[8][2] = {
        {2.0, 2.0},   {3.0, 1.0},   {2.0, 3.0},   {3.0, 3.0},
        {-2.0, -2.0}, {-3.0, -1.0}, {-2.0, -3.0}, {-3.0, -3.0},
    };
    const double ys[8] = {1.0, 1.0, 1.0, 1.0, -1.0, -1.0, -1.0, -1.0};
    for (int i = 0; i < n; ++i) {
        fx->y_storage[i] = ys[i];
        fx->nodes[i] = &fx->node_storage[(size_t)i * (size_t)nz_per_row];
        fx->nodes[i][0].index = 1;
        fx->nodes[i][0].value = xs[i][0];
        fx->nodes[i][1].index = 2;
        fx->nodes[i][1].value = xs[i][1];
        fx->nodes[i][2].index = -1;
        fx->nodes[i][2].value = 0.0;
    }
    fx->prob.y = fx->y_storage;
    fx->prob.x = fx->nodes;
    return 0;
}

static void free_binary_problem(struct binary_fixture *fx)
{
    free(fx->y_storage);
    free((void *)fx->nodes);
    free(fx->node_storage);
    memset(fx, 0, sizeof(*fx));
}

static void default_c_svc_param(struct svm_parameter *p)
{
    memset(p, 0, sizeof(*p));
    p->svm_type = C_SVC;
    p->kernel_type = LINEAR;
    p->cache_size = 16.0;
    p->eps = 1e-3;
    p->C = 1.0;
    p->shrinking = 1;
    p->probability = 0;
}

/* ------------------------------------------------------------------ */
/*  svm_check_parameter — rejection branches not in test_svm_parser    */
/* ------------------------------------------------------------------ */

static char *test_check_param_accepts_default(void)
{
    struct binary_fixture fx = {0};
    mu_assert("fixture build", build_binary_problem(&fx) == 0);
    struct svm_parameter p;
    default_c_svc_param(&p);
    const char *err = svm_check_parameter(&fx.prob, &p);
    free_binary_problem(&fx);
    mu_assert("default C-SVC param accepted", err == NULL);
    return NULL;
}

static char *test_check_param_rejects_unknown_svm_type(void)
{
    struct binary_fixture fx = {0};
    mu_assert("fixture build", build_binary_problem(&fx) == 0);
    struct svm_parameter p;
    default_c_svc_param(&p);
    p.svm_type = 99;
    const char *err = svm_check_parameter(&fx.prob, &p);
    free_binary_problem(&fx);
    mu_assert("unknown svm type rejected", err != NULL);
    mu_assert("error mentions svm type", strstr(err, "svm type") != NULL);
    return NULL;
}

static char *test_check_param_rejects_unknown_kernel(void)
{
    struct binary_fixture fx = {0};
    mu_assert("fixture build", build_binary_problem(&fx) == 0);
    struct svm_parameter p;
    default_c_svc_param(&p);
    p.kernel_type = 99;
    const char *err = svm_check_parameter(&fx.prob, &p);
    free_binary_problem(&fx);
    mu_assert("unknown kernel rejected", err != NULL);
    mu_assert("error mentions kernel", strstr(err, "kernel") != NULL);
    return NULL;
}

/* Per-case rejection helper. Builds the binary fixture, lets the
 * caller mutate `p` (already populated with the C-SVC default), runs
 * svm_check_parameter, asserts the call returned a non-NULL error
 * message, and tears down. Keeps each rejection test under the
 * readability-function-size + branch-count thresholds. */
typedef void (*param_mutator_fn)(struct svm_parameter *p);

static char *expect_param_rejection(param_mutator_fn mutate)
{
    struct binary_fixture fx = {0};
    mu_assert("fixture build", build_binary_problem(&fx) == 0);
    struct svm_parameter p;
    default_c_svc_param(&p);
    mutate(&p);
    const char *err = svm_check_parameter(&fx.prob, &p);
    free_binary_problem(&fx);
    mu_assert("expected rejection but got NULL", err != NULL);
    return NULL;
}

static void mut_cache_zero(struct svm_parameter *p)
{
    p->cache_size = 0.0;
}
static void mut_eps_zero(struct svm_parameter *p)
{
    p->eps = 0.0;
}
static void mut_c_zero(struct svm_parameter *p)
{
    p->C = 0.0;
}
static void mut_shrinking_bad(struct svm_parameter *p)
{
    p->shrinking = 2;
}
static void mut_probability_bad(struct svm_parameter *p)
{
    p->probability = 2;
}
static void mut_rbf_neg_gamma(struct svm_parameter *p)
{
    p->kernel_type = RBF;
    p->gamma = -0.1;
}
static void mut_poly_neg_degree(struct svm_parameter *p)
{
    p->kernel_type = POLY;
    p->gamma = 0.5;
    p->degree = -1;
}
static void mut_one_class_probability(struct svm_parameter *p)
{
    p->svm_type = ONE_CLASS;
    p->nu = 0.5;
    p->probability = 1;
}
static void mut_nu_svc_nu_zero(struct svm_parameter *p)
{
    p->svm_type = NU_SVC;
    p->nu = 0.0;
}
static void mut_epsilon_svr_neg_p(struct svm_parameter *p)
{
    p->svm_type = EPSILON_SVR;
    p->p = -0.5;
}

static char *test_check_param_rejects_cache_zero(void)
{
    return expect_param_rejection(mut_cache_zero);
}
static char *test_check_param_rejects_eps_zero(void)
{
    return expect_param_rejection(mut_eps_zero);
}
static char *test_check_param_rejects_c_zero(void)
{
    return expect_param_rejection(mut_c_zero);
}
static char *test_check_param_rejects_shrinking(void)
{
    return expect_param_rejection(mut_shrinking_bad);
}
static char *test_check_param_rejects_probability(void)
{
    return expect_param_rejection(mut_probability_bad);
}
static char *test_check_param_rejects_rbf_neg_gamma(void)
{
    return expect_param_rejection(mut_rbf_neg_gamma);
}
static char *test_check_param_rejects_poly_neg_degree(void)
{
    return expect_param_rejection(mut_poly_neg_degree);
}
static char *test_check_param_rejects_one_class_prob(void)
{
    return expect_param_rejection(mut_one_class_probability);
}
static char *test_check_param_rejects_nu_svc_nu_zero(void)
{
    return expect_param_rejection(mut_nu_svc_nu_zero);
}
static char *test_check_param_rejects_epsilon_svr_neg_p(void)
{
    return expect_param_rejection(mut_epsilon_svr_neg_p);
}

/* ------------------------------------------------------------------ */
/*  svm_train + inspectors + svm_predict / svm_predict_values          */
/* ------------------------------------------------------------------ */

/* Train a C-SVC on the binary fixture. Caller owns both the fixture
 * and the returned model — release via free_binary_problem +
 * svm_free_and_destroy_model. Returns NULL on training failure. */
static struct svm_model *train_default_csvc(struct binary_fixture *fx)
{
    if (build_binary_problem(fx) != 0)
        return NULL;
    struct svm_parameter p;
    default_c_svc_param(&p);
    svm_set_print_string_function(&silence_svm_log);
    if (svm_check_parameter(&fx->prob, &p) != NULL) {
        free_binary_problem(fx);
        return NULL;
    }
    return svm_train(&fx->prob, &p);
}

/* Verify all sv_indices fall in [1, l]. Out-of-line so the inspector
 * test below stays under the branch threshold. */
static int sv_indices_all_in_range(const struct svm_model *m, int l)
{
    int nsv = svm_get_nr_sv(m);
    int *sv_indices = (int *)calloc((size_t)nsv, sizeof(int));
    if (!sv_indices)
        return 0;
    svm_get_sv_indices(m, sv_indices);
    int ok = 1;
    for (int i = 0; i < nsv; ++i) {
        if (sv_indices[i] < 1 || sv_indices[i] > l) {
            ok = 0;
            break;
        }
    }
    free(sv_indices);
    return ok;
}

static char *test_train_csvc_inspectors(void)
{
    struct binary_fixture fx = {0};
    struct svm_model *m = train_default_csvc(&fx);
    mu_assert("model trained", m != NULL);
    mu_assert("svm_type == C_SVC", svm_get_svm_type(m) == C_SVC);
    mu_assert("nr_class == 2", svm_get_nr_class(m) == 2);
    mu_assert("nr_sv > 0", svm_get_nr_sv(m) > 0);
    mu_assert("sv_indices in [1, l]", sv_indices_all_in_range(m, fx.prob.l));
    /* Probability not enabled. */
    mu_assert("not a probability model", svm_check_probability_model(m) == 0);
    mu_assert("svr probability = 0", svm_get_svr_probability(m) == 0.0);
    svm_free_and_destroy_model(&m);
    free_binary_problem(&fx);
    return NULL;
}

static char *test_train_csvc_labels(void)
{
    struct binary_fixture fx = {0};
    struct svm_model *m = train_default_csvc(&fx);
    mu_assert("model trained", m != NULL);
    int labels[2] = {0, 0};
    svm_get_labels(m, labels);
    int has_pos = labels[0] == 1 || labels[1] == 1;
    int has_neg = labels[0] == -1 || labels[1] == -1;
    svm_free_and_destroy_model(&m);
    free_binary_problem(&fx);
    mu_assert("labels include +1 and -1", has_pos && has_neg);
    return NULL;
}

static char *test_train_csvc_predict(void)
{
    struct binary_fixture fx = {0};
    struct svm_model *m = train_default_csvc(&fx);
    mu_assert("model trained", m != NULL);

    /* + side */
    struct svm_node q_pos[3] = {{1, 2.5}, {2, 2.5}, {-1, 0.0}};
    double dec[1] = {0.0};
    double y_pos = svm_predict_values(m, q_pos, dec);
    /* SVM predict returns exact integer class labels (+1.0 / -1.0); these are
     * sentinel comparisons, not computed-float equality. */
    int pos_ok = (svm_predict(m, q_pos) == y_pos) && (y_pos == 1.0); /* sentinel: SVM label */

    /* - side */
    struct svm_node q_neg[3] = {{1, -2.5}, {2, -2.5}, {-1, 0.0}};
    int neg_ok = svm_predict(m, q_neg) == -1.0; /* sentinel: SVM label */

    svm_free_and_destroy_model(&m);
    int model_nulled = m == NULL;
    free_binary_problem(&fx);
    mu_assert("+ side predicts +1 (predict == predict_values)", pos_ok);
    mu_assert("- side predicts -1", neg_ok);
    mu_assert("svm_free_and_destroy_model nulls the pointer", model_nulled);
    return NULL;
}

/* Reduce per-test branch count by pulling the prob-vector validation
 * into a helper. */
static int probs_sum_to_one_in_unit(const double *probs, int n)
{
    double sum = 0.0;
    int all_in_unit = 1;
    for (int i = 0; i < n; ++i) {
        sum += probs[i];
        if (probs[i] < 0.0 || probs[i] > 1.0) {
            all_in_unit = 0;
        }
    }
    return all_in_unit && fabs(sum - 1.0) < 1e-3;
}

static char *test_predict_probability_csvc(void)
{
    struct binary_fixture fx = {0};
    mu_assert("fixture build", build_binary_problem(&fx) == 0);
    struct svm_parameter p;
    default_c_svc_param(&p);
    p.probability = 1; /* Enable Platt scaling. */
    svm_set_print_string_function(&silence_svm_log);

    struct svm_model *m = svm_train(&fx.prob, &p);
    mu_assert("probability model trained", m != NULL);
    mu_assert("flagged as probability model", svm_check_probability_model(m) == 1);

    int n = svm_get_nr_class(m);
    double *probs = (double *)calloc((size_t)n, sizeof(double));
    mu_assert("alloc probs", probs != NULL);

    struct svm_node q[3] = {{1, 2.5}, {2, 2.5}, {-1, 0.0}};
    double y = svm_predict_probability(m, q, probs);
    int label_ok = y == 1.0 || y == -1.0;
    int probs_ok = probs_sum_to_one_in_unit(probs, n);

    free(probs);
    svm_free_and_destroy_model(&m);
    free_binary_problem(&fx);
    mu_assert("predict_probability returns valid label", label_ok);
    mu_assert("probs in [0,1] and sum ~ 1.0", probs_ok);
    return NULL;
}

static char *test_train_epsilon_svr(void)
{
    /* EPSILON_SVR drives a different solver path than C_SVC + adds
     * the probA-only branch of svm_check_probability_model. */
    struct binary_fixture fx = {0};
    mu_assert("fixture build", build_binary_problem(&fx) == 0);
    /* Override y as a continuous target. */
    for (int i = 0; i < fx.prob.l; ++i) {
        fx.y_storage[i] = (i < 4) ? 1.0 : -1.0;
    }
    struct svm_parameter p;
    memset(&p, 0, sizeof(p));
    p.svm_type = EPSILON_SVR;
    p.kernel_type = LINEAR;
    p.cache_size = 16.0;
    p.eps = 1e-3;
    p.C = 1.0;
    p.p = 0.1;
    p.shrinking = 1;
    p.probability = 1;
    svm_set_print_string_function(&silence_svm_log);

    const char *err = svm_check_parameter(&fx.prob, &p);
    mu_assert("epsilon-svr param ok", err == NULL);

    struct svm_model *m = svm_train(&fx.prob, &p);
    mu_assert("svr model trained", m != NULL);
    mu_assert("svr svm_type", svm_get_svm_type(m) == EPSILON_SVR);
    /* probA only path. */
    mu_assert("svr probability model", svm_check_probability_model(m) == 1);
    /* For regression svm_get_svr_probability returns sigma > 0. */
    mu_assert("svr probability > 0", svm_get_svr_probability(m) > 0.0);

    struct svm_node q[3] = {{1, 2.0}, {2, 2.0}, {-1, 0.0}};
    double dec[1] = {0.0};
    double v = svm_predict_values(m, q, dec);
    mu_assert("svr predicts finite", isfinite(v));

    svm_free_and_destroy_model(&m);
    free_binary_problem(&fx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Save + load round-trip                                             */
/* ------------------------------------------------------------------ */

/* Inspector equality across two svm_models — type + nr_class + nr_sv +
 * one-query predict agreement. Out-of-line so the round-trip test stays
 * under the branch-count threshold. */
static int models_inspector_equal(const struct svm_model *a, const struct svm_model *b)
{
    if (svm_get_svm_type(a) != svm_get_svm_type(b))
        return 0;
    if (svm_get_nr_class(a) != svm_get_nr_class(b))
        return 0;
    if (svm_get_nr_sv(a) != svm_get_nr_sv(b))
        return 0;
    struct svm_node q[3] = {{1, 2.5}, {2, 2.5}, {-1, 0.0}};
    /* SVM predict returns an exact integer class label; comparing two calls
     * on the same input verifies that the serialised model round-trips
     * identically. Intentional exact float equality. */
    return svm_predict(a, q) == svm_predict(b, q); /* model round-trip identity */
}

/* Portable temp-path helper: MSYS2/MinGW64 on GitHub Actions does not expose
 * a usable /tmp from the MINGW64 shell; hardcoded /tmp/... templates fail with
 * ENOENT.  On _WIN32 query GetTempPathA() + embed the PID for uniqueness.
 * Returns 0 on success, -1 on failure. */
static int make_svm_temp_path(char *out, size_t out_len)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    DWORD tmplen = GetTempPathA((DWORD)sizeof(tmpdir), tmpdir);
    if (tmplen == 0 || tmplen >= (DWORD)sizeof(tmpdir))
        return -1;
    int n =
        snprintf(out, out_len, "%svmaf_svm_test_%lu", tmpdir, (unsigned long)GetCurrentProcessId());
    if (n <= 0 || (size_t)n >= out_len)
        return -1;
    /* Pre-create so svm_save_model can open it. */
    FILE *f = fopen(out, "w");
    if (!f)
        return -1;
    (void)fclose(f);
    return 0;
#else
    const char tmpl[] = "/tmp/vmaf-test-svm-XXXXXX";
    if (sizeof(tmpl) > out_len)
        return -1;
    memcpy(out, tmpl, sizeof(tmpl));
    int fd = mkstemp(out);
    if (fd < 0)
        return -1;
    (void)close(fd);
    return 0;
#endif
}

static char *test_save_load_roundtrip(void)
{
    struct binary_fixture fx = {0};
    struct svm_model *m = train_default_csvc(&fx);
    mu_assert("model trained", m != NULL);

    char path[260];
    int rc_tmp = make_svm_temp_path(path, sizeof(path));
    mu_assert("temp path creation ok", rc_tmp == 0);

    int rc = svm_save_model(path, m);
    mu_assert("svm_save_model returns 0", rc == 0);

    struct svm_model *m2 = svm_load_model(path);
    mu_assert("model reloaded", m2 != NULL);
    int eq = models_inspector_equal(m, m2);

    svm_free_and_destroy_model(&m);
    svm_free_and_destroy_model(&m2);
    (void)remove(path);
    free_binary_problem(&fx);
    mu_assert("reloaded model inspector + predict match original", eq);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Runner                                                             */
/* ------------------------------------------------------------------ */

static char *run_check_param_simple_tests(void)
{
    mu_run_test(test_check_param_accepts_default);
    mu_run_test(test_check_param_rejects_unknown_svm_type);
    mu_run_test(test_check_param_rejects_unknown_kernel);
    return NULL;
}

static char *run_check_param_numeric_tests(void)
{
    mu_run_test(test_check_param_rejects_cache_zero);
    mu_run_test(test_check_param_rejects_eps_zero);
    mu_run_test(test_check_param_rejects_c_zero);
    mu_run_test(test_check_param_rejects_shrinking);
    mu_run_test(test_check_param_rejects_probability);
    return NULL;
}

static char *run_check_param_kernel_svm_tests(void)
{
    mu_run_test(test_check_param_rejects_rbf_neg_gamma);
    mu_run_test(test_check_param_rejects_poly_neg_degree);
    mu_run_test(test_check_param_rejects_one_class_prob);
    mu_run_test(test_check_param_rejects_nu_svc_nu_zero);
    mu_run_test(test_check_param_rejects_epsilon_svr_neg_p);
    return NULL;
}

static char *run_train_predict_tests(void)
{
    mu_run_test(test_train_csvc_inspectors);
    mu_run_test(test_train_csvc_labels);
    mu_run_test(test_train_csvc_predict);
    mu_run_test(test_predict_probability_csvc);
    mu_run_test(test_train_epsilon_svr);
    return NULL;
}

char *run_tests(void)
{
    char *msg = run_check_param_simple_tests();
    if (msg)
        return msg;
    msg = run_check_param_numeric_tests();
    if (msg)
        return msg;
    msg = run_check_param_kernel_svm_tests();
    if (msg)
        return msg;
    msg = run_train_predict_tests();
    if (msg)
        return msg;
    mu_run_test(test_save_load_roundtrip);
    return NULL;
}
