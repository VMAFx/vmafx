/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression for the parse_libsvm_model memory leak found by the nightly
 *  fuzz_json_model LeakSanitizer lane (master 042c48adc7 reported
 *  "248 byte(s) leaked in 6 allocation(s)" — a direct leak of the
 *  Malloc(svm_model, 1) inside SVMModelParser::parse(), plus the sv_coef array
 *  and its rows as indirect leaks).
 *
 *  A duplicate `model` key re-enters parse_libsvm_model, and the unconditional
 *  `model->svm = svm_parse_model_from_buffer(...)` orphaned the first
 *  svm_model: nothing holds a pointer to it any more, so neither
 *  vmaf_model_destroy nor svm_free_and_destroy_model can reach it. Exactly the
 *  shape of the duplicate `feature_names` leak covered in test_model.c.
 *
 *  This lives in its own translation unit rather than beside its sibling in
 *  test_model.c on purpose: test_model.c carries 192 pre-existing clang-tidy
 *  warnings and `.clang-tidy` sets WarningsAsErrors, so the required
 *  "Clang-Tidy (Changed C/C++ Files)" job fails for any PR that merely touches
 *  it. Clearing that debt is PR #1192's job; duplicating it here would collide
 *  with that PR. CLAUDE.md §8 already directs fork-added tests to separate
 *  files.
 */

#include <stdbool.h>
#include <stddef.h>

#include "libvmaf/model.h"

#include "read_json_model.h"
#include "test.h"

/* Both payloads are minimal well-formed libsvm models, so the FIRST one really
 * is parsed and allocated before the second overwrites it — a malformed first
 * payload would return NULL and never leak, and the test would pass vacuously.
 * Under `-Db_sanitize=address` this reports a direct leak pre-fix and is clean
 * post-fix. The return code is deliberately not asserted: the regression
 * signal is the absence of a leak, not the parse verdict. */
static char *test_json_model_libsvm_duplicate_key_no_leak(void)
{
    const char json[] = "{\"model_dict\": {"
                        "\"model\": \"svm_type nu_svr\\nkernel_type linear\\nnr_class 2\\n"
                        "total_sv 1\\nrho 0.5\\nSV\\n1.0 1:1.0\\n\","
                        "\"model\": \"svm_type nu_svr\\nkernel_type linear\\nnr_class 2\\n"
                        "total_sv 1\\nrho 0.25\\nSV\\n1.0 1:2.0\\n\""
                        "}}";
    VmafModel *model = NULL;
    VmafModelConfig cfg = {0};
    const int err = vmaf_read_json_model_from_buffer(&model, &cfg, json, (int)sizeof(json) - 1);

    /* mu_assert expands to an early `return message`, so release the model
     * before asserting — otherwise a failing assertion leaks it and masks the
     * very leak this test exists to detect. */
    const bool have_model = model != NULL;
    if (have_model)
        vmaf_model_destroy(model);

    /* Ownership contract: on a non-zero return *model is left NULL; on success
     * the caller owns it. */
    if (err == 0)
        mu_assert("successful parse must yield a model", have_model);
    else
        mu_assert("rejected parse must leave *model untouched (NULL)", !have_model);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_json_model_libsvm_duplicate_key_no_leak);
    return NULL;
}
