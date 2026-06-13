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
 * Regression test for the sequential-realloc double-free fixed in PR #708.
 *
 * Background: PR #658 introduced a double-free in svm_group_classes() and
 * svm_check_parameter() — when realloc(label) succeeded (freeing the old
 * block internally), a subsequent failure in realloc(count) would then
 * call free(label) on the now-dangling original pointer.  PR #708 fixed
 * both sites by separating the two realloc calls so each is checked
 * independently before the next one is attempted.
 *
 * The realloc path is only reached when the number of distinct class labels
 * exceeds max_nr_class (= 16 initial).  The existing test_svm_api.c uses a
 * 2-class fixture which never reaches the doubling threshold.  This file
 * supplies a 17-class and a 32-class fixture to exercise the realloc branch
 * under ASan/UBSan — any double-free or use-after-free regresses as an
 * immediate abort in sanitized builds.
 *
 * svm_check_parameter()'s realloc path is inside the NU_SVC feasibility
 * check; a 17-class NU_SVC fixture drives it.
 *
 * Note: svm_group_classes() is static to svm.cpp and is not directly
 * callable from C.  The public entry point that calls it is svm_train();
 * training a 17-class C_SVC problem exercises the path indirectly.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svm.h"
#include "test.h"

static void silence_svm_log(const char *s)
{
    (void)s;
}

/* ------------------------------------------------------------------ */
/* N-class fixture builder (one sample per class, 1 feature)          */
/* ------------------------------------------------------------------ */

struct nclass_fixture {
    struct svm_problem prob;
    double *y_storage;
    struct svm_node **x_storage;
    struct svm_node *node_storage; /* n * 2 nodes per sample (feat + sentinel) */
};

/* Build a problem with n distinct integer classes 0..n-1, one sample per
 * class, one feature = the class index (clearly linearly separable).
 * The caller must call free_nclass_fixture() when done.
 * Returns 0 on success, -1 on allocation failure. */
static int build_nclass_fixture(struct nclass_fixture *fx, int n)
{
    const int nodes_per_row = 2; /* feature[0] + sentinel[-1] */
    fx->prob.l = n;
    fx->y_storage = (double *)calloc((size_t)n, sizeof(double));
    fx->x_storage = (struct svm_node **)calloc((size_t)n, sizeof(struct svm_node *));
    fx->node_storage =
        (struct svm_node *)calloc((size_t)n * (size_t)nodes_per_row, sizeof(struct svm_node));
    if (!fx->y_storage || !fx->x_storage || !fx->node_storage) {
        free(fx->y_storage);
        free((void *)fx->x_storage);
        free(fx->node_storage);
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        fx->y_storage[i] = (double)i; /* class label = i */
        fx->x_storage[i] = &fx->node_storage[(size_t)i * (size_t)nodes_per_row];
        fx->x_storage[i][0].index = 1;
        fx->x_storage[i][0].value = (double)i;
        fx->x_storage[i][1].index = -1;
        fx->x_storage[i][1].value = 0.0;
    }
    fx->prob.y = fx->y_storage;
    fx->prob.x = fx->x_storage;
    return 0;
}

static void free_nclass_fixture(struct nclass_fixture *fx)
{
    free(fx->y_storage);
    free((void *)fx->x_storage);
    free(fx->node_storage);
    memset(fx, 0, sizeof(*fx));
}

/* ------------------------------------------------------------------ */
/* svm_group_classes realloc path — exercised via svm_train           */
/* ------------------------------------------------------------------ */

/* 17 classes: forces the max_nr_class=16 → 32 realloc doubling in
 * svm_group_classes().  Under ASan any double-free aborts immediately. */
static char *test_train_17class_csvc_exercises_realloc(void)
{
    struct nclass_fixture fx = {0};
    mu_assert("17-class fixture build", build_nclass_fixture(&fx, 17) == 0);

    struct svm_parameter p;
    memset(&p, 0, sizeof(p));
    p.svm_type = C_SVC;
    p.kernel_type = LINEAR;
    p.cache_size = 16.0;
    p.eps = 1e-3;
    p.C = 1.0;
    p.shrinking = 1;
    p.probability = 0;

    svm_set_print_string_function(&silence_svm_log);
    const char *chk = svm_check_parameter(&fx.prob, &p);
    mu_assert("17-class C_SVC param accepted", chk == NULL);

    struct svm_model *m = svm_train(&fx.prob, &p);
    mu_assert("17-class model trained (realloc path exercised)", m != NULL);
    mu_assert("nr_class == 17", svm_get_nr_class(m) == 17);
    mu_assert("nr_sv > 0", svm_get_nr_sv(m) > 0);

    /* Predict a point for class 8 — should be closest centroid. */
    struct svm_node q[2] = {{1, 8.0}, {-1, 0.0}};
    (void)svm_predict(m, q);

    svm_free_and_destroy_model(&m);
    free_nclass_fixture(&fx);
    return NULL;
}

/* 32 classes: exercises a second realloc doubling (32 → 64) inside
 * svm_group_classes(), the same sequential pattern at svm.cpp line 1995/2003.
 */
static char *test_train_32class_csvc_exercises_second_realloc(void)
{
    struct nclass_fixture fx = {0};
    mu_assert("32-class fixture build", build_nclass_fixture(&fx, 32) == 0);

    struct svm_parameter p;
    memset(&p, 0, sizeof(p));
    p.svm_type = C_SVC;
    p.kernel_type = LINEAR;
    p.cache_size = 32.0;
    p.eps = 1e-3;
    p.C = 1.0;
    p.shrinking = 1;
    p.probability = 0;

    svm_set_print_string_function(&silence_svm_log);
    const char *chk = svm_check_parameter(&fx.prob, &p);
    mu_assert("32-class C_SVC param accepted", chk == NULL);

    struct svm_model *m = svm_train(&fx.prob, &p);
    mu_assert("32-class model trained (second realloc path exercised)", m != NULL);
    mu_assert("nr_class == 32", svm_get_nr_class(m) == 32);

    svm_free_and_destroy_model(&m);
    free_nclass_fixture(&fx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* svm_check_parameter realloc path — NU_SVC with 17 classes          */
/* ------------------------------------------------------------------ */

/* The realloc in svm_check_parameter is inside the NU_SVC feasibility
 * check (svm.cpp ~line 2797/2805).  Build a 17-class NU_SVC problem
 * with nu large enough to be feasible (nu < min_class_frac). */
static char *test_check_param_nusvc_17class_exercises_realloc(void)
{
    /* 2 samples per class keeps the per-class count balanced.
     * nu must satisfy: nu * (n1 + n2) / 2 <= min(n1, n2) = 2
     * i.e. nu * 2 <= 2 => nu <= 1.0.  Use nu=0.5 for headroom. */
    const int n_classes = 17;
    const int samples_per_class = 2;
    const int n = n_classes * samples_per_class;
    const int nodes_per_row = 2;

    double *y = (double *)calloc((size_t)n, sizeof(double));
    struct svm_node **x = (struct svm_node **)calloc((size_t)n, sizeof(struct svm_node *));
    struct svm_node *nodes =
        (struct svm_node *)calloc((size_t)n * (size_t)nodes_per_row, sizeof(struct svm_node));
    mu_assert("alloc y", y != NULL);
    mu_assert("alloc x", x != NULL);
    mu_assert("alloc nodes", nodes != NULL);

    for (int i = 0; i < n; ++i) {
        int cls = i / samples_per_class;
        y[i] = (double)cls;
        x[i] = &nodes[(size_t)i * (size_t)nodes_per_row];
        x[i][0].index = 1;
        x[i][0].value = (double)cls;
        x[i][1].index = -1;
        x[i][1].value = 0.0;
    }

    struct svm_problem prob;
    prob.l = n;
    prob.y = y;
    prob.x = x;

    struct svm_parameter p;
    memset(&p, 0, sizeof(p));
    p.svm_type = NU_SVC;
    p.kernel_type = LINEAR;
    p.cache_size = 16.0;
    p.eps = 1e-3;
    p.nu = 0.5; /* feasible: 0.5 * (2+2)/2 = 1.0 <= min(2,2) = 2 */
    p.shrinking = 1;
    p.probability = 0;

    svm_set_print_string_function(&silence_svm_log);
    /* svm_check_parameter with NU_SVC iterates all classes and allocates
     * label/count buffers starting at max_nr_class=16.  With 17 classes
     * it triggers the realloc doubling at svm.cpp line 2797. */
    const char *err = svm_check_parameter(&prob, &p);
    mu_assert("nu-svc 17-class realloc path: check_parameter returns NULL (feasible)", err == NULL);

    free(y);
    free((void *)x);
    free(nodes);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

char *run_tests(void)
{
    mu_run_test(test_train_17class_csvc_exercises_realloc);
    mu_run_test(test_train_32class_csvc_exercises_second_realloc);
    mu_run_test(test_check_param_nusvc_17class_exercises_realloc);
    return NULL;
}
