/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Netflix/vmaf#1242 regression test — the VmafFeatureDictionary ownership
 *  contract of vmaf_model_feature_overload() and
 *  vmaf_model_collection_feature_overload().
 *
 *  Three defects were reported / found:
 *
 *    1. vmaf_model_feature_overload() returned `-ENOMEM` straight out of the
 *       loop when vmaf_dictionary_merge() failed, skipping the unconditional
 *       `vmaf_dictionary_free(&opts_dict)` at the tail — an unconditional leak
 *       of the caller's dictionary under the documented "the call consumes it"
 *       contract.  Now the loop breaks and falls through to the common exit.
 *       `test_overload_merge_failure_consumes_dict` drives that exact branch
 *       without malloc-fail injection; the leak itself is what LeakSanitizer
 *       reports in the ASan lane.
 *
 *    2. vmaf_model_collection_feature_overload() discarded
 *       vmaf_dictionary_copy()'s return value, leaked the partially built copy
 *       and silently skipped the remaining sub-models while still able to
 *       report success.  It also dereferenced `*model_collection` without
 *       checking it, and never checked `model` / `feature_name` / `opts_dict`.
 *       `test_collection_overload_rejects_null_collection_handle` below is
 *       undefined behaviour on the pre-fix tree (a NULL dereference) and a
 *       defined -EINVAL after.
 *
 *    3. <libvmaf/feature.h> ("on failure the caller still owns the
 *       dictionary") and <libvmaf/model.h> ("ownership transfers on both
 *       success and failure") documented opposite contracts.  Both now state
 *       the implemented rule: the argument-validation guards consume nothing,
 *       every other path consumes.  The guard cases below pin that half, and
 *       are what core/test/test_model_collection_api.c already relies on.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

#include "dict.h"
#include "model.h" /* internal: VmafModel layout, for the override assertion */
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/src/feature/float_motion.c. ADR-1138. */

/* Build a single-entry options dictionary. */
static VmafFeatureDictionary *make_dict(void)
{
    VmafFeatureDictionary *dict = NULL;
    if (vmaf_feature_dictionary_set(&dict, "adm_enhancement_gain_limit", "1.1"))
        return NULL;
    return dict;
}

/* ------------------------------------------------------------------ */
/* Argument-validation guards must NOT consume the dictionary.        */
/* ------------------------------------------------------------------ */

static char *test_overload_guards_do_not_consume(void)
{
    VmafFeatureDictionary *dict = make_dict();
    mu_assert("vmaf_feature_dictionary_set failed", dict != NULL);

    mu_assert("overload(NULL model) must return -EINVAL",
              vmaf_model_feature_overload(NULL, "adm", dict) == -EINVAL);

    /* The dictionary must still be intact and owned by us. */
    VmafDictionaryEntry *e =
        vmaf_dictionary_get((VmafDictionary **)&dict, "adm_enhancement_gain_limit", 0);
    mu_assert("guard path must leave the dictionary intact", e != NULL);
    mu_assert("guard path must leave the value intact", strcmp(e->val, "1.1") == 0);

    /* ... and releasing it here must be well-defined (not a double free). */
    mu_assert("caller must be able to free after a guard rejection",
              vmaf_feature_dictionary_free(&dict) == 0);
    return NULL;
}

static char *test_overload_null_feature_name_guard(void)
{
    VmafFeatureDictionary *dict = make_dict();
    mu_assert("vmaf_feature_dictionary_set failed", dict != NULL);

    VmafModel *model = NULL;
    VmafModelConfig cfg = {0};
    mu_assert("vmaf_model_load failed", vmaf_model_load(&model, &cfg, "vmaf_v0.6.1") == 0);

    mu_assert("overload(NULL feature_name) must return -EINVAL",
              vmaf_model_feature_overload(model, NULL, dict) == -EINVAL);
    mu_assert("caller must be able to free after a guard rejection",
              vmaf_feature_dictionary_free(&dict) == 0);

    vmaf_model_destroy(model);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Collection wrapper: a NULL collection behind a non-NULL handle must */
/* be rejected, not dereferenced.                                      */
/* ------------------------------------------------------------------ */

static char *test_collection_overload_rejects_null_collection_handle(void)
{
    VmafFeatureDictionary *dict = make_dict();
    mu_assert("vmaf_feature_dictionary_set failed", dict != NULL);

    VmafModel *model = NULL;
    VmafModelConfig cfg = {0};
    mu_assert("vmaf_model_load failed", vmaf_model_load(&model, &cfg, "vmaf_v0.6.1") == 0);

    /* Pre-fix this reached `mc->cnt` with mc == NULL: undefined behaviour that
     * the optimiser is free to (and, under LTO, does) delete, leaving the
     * function returning garbage.  UBSan/ASan trap on it; post-fix it is a
     * defined -EINVAL on every build. */
    VmafModelCollection *mc = NULL;
    mu_assert("collection overload(*model_collection == NULL) must return -EINVAL",
              vmaf_model_collection_feature_overload(model, &mc, "adm", dict) == -EINVAL);
    mu_assert("caller must be able to free after a guard rejection",
              vmaf_feature_dictionary_free(&dict) == 0);

    vmaf_model_destroy(model);
    return NULL;
}

static char *test_collection_overload_null_lead_model_guard(void)
{
    VmafFeatureDictionary *dict = make_dict();
    mu_assert("vmaf_feature_dictionary_set failed", dict != NULL);

    VmafModelCollection *mc = NULL;
    mu_assert("collection overload(NULL model) must return -EINVAL",
              vmaf_model_collection_feature_overload(NULL, &mc, "adm", dict) == -EINVAL);
    mu_assert("collection overload(NULL handle) must return -EINVAL",
              vmaf_model_collection_feature_overload(NULL, NULL, "adm", dict) == -EINVAL);
    mu_assert("caller must be able to free after a guard rejection",
              vmaf_feature_dictionary_free(&dict) == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Success path: the dictionary is consumed and the override lands.    */
/* Never freed here — doing so would be a double free under the fixed  */
/* contract, and LSan proves the callee released it.                   */
/* ------------------------------------------------------------------ */

static char *test_overload_success_consumes_dict(void)
{
    VmafFeatureDictionary *dict = make_dict();
    mu_assert("vmaf_feature_dictionary_set failed", dict != NULL);

    VmafModel *model = NULL;
    VmafModelConfig cfg = {0};
    mu_assert("vmaf_model_load failed", vmaf_model_load(&model, &cfg, "vmaf_v0.6.1") == 0);

    mu_assert("overload must succeed", vmaf_model_feature_overload(model, "adm", dict) == 0);

    VmafDictionaryEntry *e =
        vmaf_dictionary_get(&model->feature[0].opts_dict, "adm_enhancement_gain_limit", 0);
    mu_assert("override must land on the model's feature", e != NULL);
    mu_assert("override value must be the one supplied", strcmp(e->val, "1.1") == 0);

    vmaf_model_destroy(model);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The merge-failure branch must still consume the dictionary.         */
/*                                                                     */
/* vmaf_dictionary_merge() returns NULL both on allocation failure and */
/* when there is nothing to merge, and vmaf_model_feature_overload()   */
/* maps that to -ENOMEM.  Handing it a heap-allocated EMPTY dictionary */
/* reaches that branch deterministically, with no malloc-fail          */
/* injection: pre-fix it `return`ed straight out and leaked the        */
/* caller's dictionary (the defect Netflix/vmaf#1242 reports);         */
/* post-fix it breaks to the common exit and releases it.  The return  */
/* code is -ENOMEM either way, so the leak half is what LeakSanitizer  */
/* sees in the ASan lane — hence this case must NOT free the dict.     */
/* ------------------------------------------------------------------ */

static char *test_overload_merge_failure_consumes_dict(void)
{
    VmafModel *model = NULL;
    VmafModelConfig cfg = {0};
    mu_assert("vmaf_model_load failed", vmaf_model_load(&model, &cfg, "vmaf_v0.6.1") == 0);

    VmafDictionary *empty = calloc(1, sizeof(*empty));
    mu_assert("calloc failed", empty != NULL);

    mu_assert("overload must report -ENOMEM when the merge yields nothing",
              vmaf_model_feature_overload(model, "adm", (VmafFeatureDictionary *)empty) == -ENOMEM);

    vmaf_model_destroy(model);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_overload_guards_do_not_consume);
    mu_run_test(test_overload_null_feature_name_guard);
    mu_run_test(test_collection_overload_rejects_null_collection_handle);
    mu_run_test(test_collection_overload_null_lead_model_guard);
    mu_run_test(test_overload_success_consumes_dict);
    mu_run_test(test_overload_merge_failure_consumes_dict);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
