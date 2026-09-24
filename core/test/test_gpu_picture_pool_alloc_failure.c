/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#define VMAFX_TEST_GPU_PICTURE_POOL_NO_INTERPOSE 1
#undef malloc
#undef free

#include <errno.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */
#include <stdlib.h>

#include "gpu_picture_pool.h"
#include "test.h"
#include "test_gpu_picture_pool_alloc_interpose.h"

static unsigned alloc_calls;
static unsigned fail_alloc_at;
static unsigned free_calls;

void *vmafx_test_gpu_pool_malloc(size_t size)
{
    alloc_calls++;
    if (fail_alloc_at != 0 && alloc_calls == fail_alloc_at)
        return NULL;
    return malloc(size);
}

void vmafx_test_gpu_pool_free(void *ptr)
{
    if (ptr)
        free_calls++;
    free(ptr);
}

static int stub_alloc_unreached(VmafPicture *pic, void *cookie)
{
    (void)pic;
    (void)cookie;
    return -ENOMEM;
}

static int stub_free_noop(VmafPicture *pic, void *cookie)
{
    (void)pic;
    (void)cookie;
    return 0;
}

static char sentinel_storage;
#define SENTINEL ((VmafGpuPicturePool *)&sentinel_storage)

static char *test_alloc_failure_pool_struct(void)
{
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 2,
        .cookie = NULL,
        .alloc_picture_callback = stub_alloc_unreached,
        .free_picture_callback = stub_free_noop,
        .synchronize_picture_callback = NULL,
    };

    alloc_calls = 0;
    free_calls = 0;
    fail_alloc_at = 1;
    VmafGpuPicturePool *pool = SENTINEL;
    int err = vmaf_gpu_picture_pool_init(&pool, cfg);
    fail_alloc_at = 0;

    mu_assert("init must fail with -ENOMEM when pool struct allocation fails", err == -ENOMEM);
    mu_assert("pool handle must be cleared to NULL on struct alloc failure", pool == NULL);
    mu_assert("allocator must be called once", alloc_calls == 1);
    mu_assert("free must not be called when initial allocation fails", free_calls == 0);
    return NULL;
}

static char *test_alloc_failure_pic_array(void)
{
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 2,
        .cookie = NULL,
        .alloc_picture_callback = stub_alloc_unreached,
        .free_picture_callback = stub_free_noop,
        .synchronize_picture_callback = NULL,
    };

    alloc_calls = 0;
    free_calls = 0;
    fail_alloc_at = 2;
    VmafGpuPicturePool *pool = SENTINEL;
    int err = vmaf_gpu_picture_pool_init(&pool, cfg);
    fail_alloc_at = 0;

    mu_assert("init must fail with -ENOMEM when pic array allocation fails", err == -ENOMEM);
    mu_assert("pool handle must be cleared to NULL on pic array alloc failure", pool == NULL);
    mu_assert("allocator must be called twice", alloc_calls == 2);
    mu_assert("partially constructed pool struct must be freed", free_calls == 1);
    return NULL;
}

static char *test_alloc_success_and_close(void)
{
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 2,
        .cookie = NULL,
        .alloc_picture_callback = stub_free_noop,
        .free_picture_callback = stub_free_noop,
        .synchronize_picture_callback = NULL,
    };

    alloc_calls = 0;
    free_calls = 0;
    fail_alloc_at = 0;
    VmafGpuPicturePool *pool = SENTINEL;
    int err = vmaf_gpu_picture_pool_init(&pool, cfg);

    mu_assert("init must succeed when allocations succeed", err == 0);
    mu_assert("pool handle must be non-NULL on success", pool != NULL && pool != SENTINEL);
    mu_assert("allocator must be called twice", alloc_calls == 2);

    int close_err = vmaf_gpu_picture_pool_close(pool);
    mu_assert("pool close must succeed", close_err == 0);
    mu_assert("both pool struct and pic array must be freed on close", free_calls == 2);
    return NULL;
}

static char *test_alloc_failure_invalid_config(void)
{
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 0,
        .cookie = NULL,
        .alloc_picture_callback = stub_alloc_unreached,
        .free_picture_callback = stub_free_noop,
        .synchronize_picture_callback = NULL,
    };

    alloc_calls = 0;
    free_calls = 0;
    fail_alloc_at = 0;
    VmafGpuPicturePool *pool = SENTINEL;
    int err = vmaf_gpu_picture_pool_init(&pool, cfg);

    mu_assert("init must return -EINVAL on invalid config", err == -EINVAL);
    mu_assert("early-return EINVAL must not touch *pool", pool == SENTINEL);
    mu_assert("allocator must not be called on invalid config", alloc_calls == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_alloc_failure_pool_struct);
    mu_run_test(test_alloc_failure_pic_array);
    mu_run_test(test_alloc_success_and_close);
    mu_run_test(test_alloc_failure_invalid_config);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
