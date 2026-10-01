/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Growth of a model collection, and its failure
 * (T-MODEL-COLLECTION-GROWTH-FAILURE-UNTESTED-2026-10-01).
 *
 * vmaf_model_collection_append() starts a collection with room for eight
 * models and doubles the array when it is full. When that realloc() fails,
 * the old array is still valid, so the function has to return -ENOMEM and
 * leave the collection exactly as it was. Upstream Netflix/vmaf takes its
 * common failure label there instead, which clears the caller's handle: the
 * collection and its eight models are lost, and the caller cannot tell.
 *
 * The fork has returned directly since the model-collection rework, and
 * nothing tested it. These cases drive the real library object through a
 * `-Wl,--wrap=realloc` shim that fails one chosen growth, the same control
 * test_registration_partial_copy and test_fex_pool_growth use.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "test.h"

#include "libvmaf/model.h"
#include "model.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`, and this
 * file mirrors the C spelling of the surface it exercises. ADR-1138. */

/* vmaf_model_collection_append() allocates room for this many models first. */
#define INITIAL_CAPACITY 8u

static const void *watched_array;
static bool fail_watched_growth;
static unsigned watched_growths;

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ADR-0723; Research-2047: GNU linker wrapping ABI.
extern void *__real_realloc(void *, size_t);

/* The library is compiled `-fvisibility=hidden`, which applies to this test
 * too, and the linker must reach the wrapper (test_fex_pool_growth.c). */
#define VMAF_WRAP_EXPORT __attribute__((visibility("default")))

/* Counts every growth of the watched model array and fails it on request.
 * Every other realloc() in the process runs unchanged. */
// cppcheck-suppress unusedFunction
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) — ADR-0723; Research-2047: GNU linker calls this entry point.
VMAF_WRAP_EXPORT void *__wrap_realloc(void *old, size_t size)
{
    if (old && old == watched_array) {
        watched_growths++;
        if (fail_watched_growth) {
            fail_watched_growth = false;
            return NULL;
        }
    }
    return __real_realloc(old, size);
}

static VmafModel *load_member(void)
{
    VmafModel *model = NULL;
    VmafModelConfig cfg = {.name = "growth_0000"};
    const int err = vmaf_model_load_from_path(&model, &cfg, JSON_MODEL_PATH "vmaf_v0.6.1.json");
    return err ? NULL : model;
}

/* Appends `count` freshly loaded models. A model the collection did not take
 * still belongs to this function, which destroys it. */
static int append_members(VmafModelCollection **collection, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        VmafModel *model = load_member();
        if (!model) {
            return -ENOMEM;
        }
        const int err = vmaf_model_collection_append(collection, model);
        if (err) {
            vmaf_model_destroy(model);
            return err;
        }
    }
    return 0;
}

static void watch(const VmafModelCollection *collection)
{
    watched_array = collection ? (const void *)collection->model : NULL;
    watched_growths = 0;
    fail_watched_growth = false;
}

/* boundary: the eighth model fills the initial array without growing it. */
static char *test_initial_capacity_is_filled_without_growing(void)
{
    VmafModelCollection *collection = NULL;
    watch(NULL);
    mu_assert("first append failed", !append_members(&collection, 1u));
    mu_assert("first append did not create the collection", collection);
    watch(collection);
    mu_assert("appends up to the initial capacity failed",
              !append_members(&collection, INITIAL_CAPACITY - 1u));
    const bool full = collection->cnt == INITIAL_CAPACITY && collection->size == INITIAL_CAPACITY;
    const bool grew = watched_growths != 0u;
    vmaf_model_collection_destroy(collection);
    mu_assert("eight models must exactly fill the initial array", full);
    mu_assert("the array grew before it was full", !grew);
    return NULL;
}

/* positive: the ninth model doubles the array and keeps every member. */
static char *test_ninth_model_doubles_the_array(void)
{
    VmafModelCollection *collection = NULL;
    watch(NULL);
    mu_assert("filling the initial capacity failed",
              !append_members(&collection, INITIAL_CAPACITY));
    VmafModel *const first = collection->model[0];
    VmafModel *const last = collection->model[INITIAL_CAPACITY - 1u];
    watch(collection);
    const int err = append_members(&collection, 1u);
    const bool grown = !err && watched_growths == 1u && collection->cnt == INITIAL_CAPACITY + 1u &&
                       collection->size == 2u * INITIAL_CAPACITY;
    const bool kept =
        collection->model[0] == first && collection->model[INITIAL_CAPACITY - 1u] == last;
    vmaf_model_collection_destroy(collection);
    mu_assert("the ninth append must grow the array once, to sixteen slots", grown);
    mu_assert("growing the array lost or reordered a member", kept);
    return NULL;
}

/* negative: a failed growth reports -ENOMEM and changes nothing. */
static char *test_growth_failure_keeps_the_collection(void)
{
    VmafModelCollection *collection = NULL;
    watch(NULL);
    mu_assert("filling the initial capacity failed",
              !append_members(&collection, INITIAL_CAPACITY));
    VmafModelCollection *const original = collection;
    VmafModel **const members = collection->model;
    VmafModel *const first = members[0];
    VmafModel *next = load_member();
    if (!next) {
        vmaf_model_collection_destroy(collection);
        return "model load failed";
    }

    watch(collection);
    fail_watched_growth = true;
    const int err = vmaf_model_collection_append(&collection, next);
    const bool intact = collection == original && collection->model == members &&
                        collection->cnt == INITIAL_CAPACITY &&
                        collection->size == INITIAL_CAPACITY && collection->model[0] == first;

    /* The failed append left `next` with the caller. Appending it again must
     * succeed once, so that destroying the collection frees all nine. */
    const int retry_err = intact ? vmaf_model_collection_append(&collection, next) : -EINVAL;
    const bool retried = !retry_err && collection->cnt == INITIAL_CAPACITY + 1u &&
                         collection->model[INITIAL_CAPACITY] == next;
    if (retry_err) {
        vmaf_model_destroy(next);
    }
    vmaf_model_collection_destroy(intact ? collection : original);

    mu_assert("a failed growth must be reported as -ENOMEM", err == -ENOMEM);
    mu_assert("a failed growth lost or changed the existing collection", intact);
    mu_assert("the rejected model could not be appended on retry", retried);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_initial_capacity_is_filled_without_growing);
    mu_run_test(test_ninth_model_doubles_the_array);
    mu_run_test(test_growth_failure_keeps_the_collection);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
