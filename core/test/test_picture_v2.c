/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/**
 * @file test_picture_v2.c
 *
 * Unit tests for the VmafPicture v2 API (ADR-0928 cycle N+1).
 * Covers: vmaf_picture2_alloc / vmaf_picture2_unref,
 *         vmaf_picture_v1_to_v2 / vmaf_picture_v2_to_v1,
 *         vmaf_backend_handle_name.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
#include "mu_table.h"
#include "picture.h"
#include "ref.h"
#include "libvmaf/picture.h"
#include "libvmaf/picture_v2.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* ------------------------------------------------------------------ */
/* vmaf_backend_handle_name                                            */
/* ------------------------------------------------------------------ */

static char *test_backend_handle_name_known_values(void)
{
    mu_assert("NONE maps to 'none'",
              strcmp(vmaf_backend_handle_name(VMAF_BACKEND_HANDLE_NONE), "none") == 0);
    mu_assert("CUDA maps to 'cuda'",
              strcmp(vmaf_backend_handle_name(VMAF_BACKEND_HANDLE_CUDA), "cuda") == 0);
    mu_assert("SYCL maps to 'sycl'",
              strcmp(vmaf_backend_handle_name(VMAF_BACKEND_HANDLE_SYCL), "sycl") == 0);
    mu_assert("HIP maps to 'hip'",
              strcmp(vmaf_backend_handle_name(VMAF_BACKEND_HANDLE_HIP), "hip") == 0);
    mu_assert("METAL maps to 'metal'",
              strcmp(vmaf_backend_handle_name(VMAF_BACKEND_HANDLE_METAL), "metal") == 0);
    return NULL;
}

static char *test_backend_handle_name_out_of_range(void)
{
    /* Sentinel value VMAF_BACKEND_HANDLE__COUNT is out of range. */
    mu_assert("sentinel maps to 'unknown'",
              strcmp(vmaf_backend_handle_name(VMAF_BACKEND_HANDLE__COUNT), "unknown") == 0);
    /* Large value is also out of range. */
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) — the test's subject is an out-of-range handle (ADR-0141) */
    const VmafBackendHandle out_of_range = (VmafBackendHandle)9999;
    mu_assert("large value maps to 'unknown'",
              strcmp(vmaf_backend_handle_name(out_of_range), "unknown") == 0);
    /* Never returns NULL. */
    mu_assert("return is never NULL", vmaf_backend_handle_name(VMAF_BACKEND_HANDLE_NONE) != NULL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* vmaf_picture2_alloc / vmaf_picture2_unref                          */
/* ------------------------------------------------------------------ */

/* Field checks that follow a fresh vmaf_picture2_alloc(). Split out of
 * test_picture2_alloc_and_unref so that function stays within the
 * readability-function-size branch budget (each mu_assert is two branches,
 * ADR-0141). */
static char *check_picture2_alloc_fields(const VmafPicture2 *pic)
{
    mu_assert("backend must be NONE for CPU alloc", pic->backend == VMAF_BACKEND_HANDLE_NONE);
    mu_assert("backend_handle must be 0 for CPU alloc", pic->backend_handle == 0);
    mu_assert("ref count must be 1 after alloc", vmaf_ref_load(pic->ref) == 1);
    mu_assert("data[0] must be non-NULL", pic->data[0] != NULL);
    mu_assert("data[1] must be non-NULL for YUV420", pic->data[1] != NULL);
    mu_assert("data[2] must be non-NULL for YUV420", pic->data[2] != NULL);
    mu_assert("pix_fmt carried through", pic->pix_fmt == VMAF_PIX_FMT_YUV420P);
    return NULL;
}

static char *check_picture2_alloc_dims(const VmafPicture2 *pic)
{
    mu_assert("bpc carried through", pic->bpc == 8);
    mu_assert("luma width carried through", pic->w[0] == 1920);
    mu_assert("luma height carried through", pic->h[0] == 1080);
    return NULL;
}

static char *test_picture2_alloc_and_unref(void)
{
    int err;
    VmafPicture2 pic;

    err = vmaf_picture2_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 1080);
    mu_assert("vmaf_picture2_alloc failed", !err);

    char *msg = check_picture2_alloc_fields(&pic);
    if (msg)
        return msg;
    msg = check_picture2_alloc_dims(&pic);
    if (msg)
        return msg;

    err = vmaf_picture2_unref(&pic);
    mu_assert("vmaf_picture2_unref failed", !err);
    /* After unref the struct is zeroed. */
    mu_assert("ref must be NULL after last unref", pic.ref == NULL);
    return NULL;
}

static char *test_picture2_alloc_null_rejected(void)
{
    int err = vmaf_picture2_alloc(NULL, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    mu_assert("NULL pic must return -EINVAL", err == -EINVAL);
    return NULL;
}

static char *test_picture2_unref_null_noop(void)
{
    /* NULL is documented as a no-op. */
    int err = vmaf_picture2_unref(NULL);
    mu_assert("vmaf_picture2_unref(NULL) must succeed", err == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* vmaf_picture_v1_to_v2                                              */
/* ------------------------------------------------------------------ */

/* The post-promotion field checks for test_v1_to_v2_basic, split out for the
 * same branch-budget reason as check_picture2_alloc_fields above. */
static char *check_v1_to_v2_fields(const VmafPicture *v1, const VmafPicture2 *v2)
{
    /* Both v1 and v2 now hold a reference — count is 2. */
    mu_assert("ref count must be 2 after v1→v2 promotion", vmaf_ref_load(v1->ref) == 2);
    mu_assert("v2.ref must equal v1.ref", v2->ref == v1->ref);
    mu_assert("v2.data[0] must equal v1.data[0]", v2->data[0] == v1->data[0]);
    mu_assert("v2.backend must be NONE", v2->backend == VMAF_BACKEND_HANDLE_NONE);
    mu_assert("v2.backend_handle must be 0", v2->backend_handle == 0);
    mu_assert("v2.w[0] carried", v2->w[0] == 320);
    mu_assert("v2.h[0] carried", v2->h[0] == 240);
    return NULL;
}

static char *test_v1_to_v2_basic(void)
{
    int err;
    VmafPicture v1;
    VmafPicture2 v2;

    err = vmaf_picture_alloc(&v1, VMAF_PIX_FMT_YUV420P, 8, 320, 240);
    mu_assert("v1 alloc failed", !err);
    mu_assert("v1 ref count is 1 before promotion", vmaf_ref_load(v1.ref) == 1);

    err = vmaf_picture_v1_to_v2(&v1, &v2);
    mu_assert("v1_to_v2 failed", !err);

    char *msg = check_v1_to_v2_fields(&v1, &v2);
    if (msg)
        return msg;

    /* Release both references independently. */
    err = vmaf_picture2_unref(&v2);
    mu_assert("v2 unref failed", !err);
    mu_assert("ref count back to 1 after v2 unref", vmaf_ref_load(v1.ref) == 1);
    err = vmaf_picture_unref(&v1);
    mu_assert("v1 unref failed", !err);
    return NULL;
}

static char *test_v1_to_v2_null_args(void)
{
    VmafPicture v1;
    VmafPicture2 v2;
    int err;

    memset(&v1, 0, sizeof(v1));
    memset(&v2, 0, sizeof(v2));

    err = vmaf_picture_v1_to_v2(NULL, &v2);
    mu_assert("NULL src must return -EINVAL", err == -EINVAL);

    err = vmaf_picture_v1_to_v2(&v1, NULL);
    mu_assert("NULL dst must return -EINVAL", err == -EINVAL);

    /* Zeroed v1 (ref == NULL) must also be rejected. */
    err = vmaf_picture_v1_to_v2(&v1, &v2);
    mu_assert("zeroed v1 (no ref) must return -EINVAL", err == -EINVAL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* vmaf_picture_v2_to_v1                                              */
/* ------------------------------------------------------------------ */

/* The post-demotion field checks for test_v2_to_v1_basic, split out for the
 * same branch-budget reason as check_picture2_alloc_fields above. */
static char *check_v2_to_v1_fields(const VmafPicture2 *v2, const VmafPicture *v1)
{
    /* Both v2 and v1 now hold a reference — count is 2. */
    mu_assert("ref count must be 2 after v2→v1 demotion", vmaf_ref_load(v2->ref) == 2);
    mu_assert("v1.ref must equal v2.ref", v1->ref == v2->ref);
    mu_assert("v1.data[0] must equal v2.data[0]", v1->data[0] == v2->data[0]);
    mu_assert("v1.pix_fmt carried", v1->pix_fmt == VMAF_PIX_FMT_YUV444P);
    mu_assert("v1.bpc carried", v1->bpc == 10);
    mu_assert("v1.w[0] carried", v1->w[0] == 160);
    mu_assert("v1.h[0] carried", v1->h[0] == 90);
    return NULL;
}

static char *test_v2_to_v1_basic(void)
{
    int err;
    VmafPicture2 v2;
    VmafPicture v1;

    err = vmaf_picture2_alloc(&v2, VMAF_PIX_FMT_YUV444P, 10, 160, 90);
    mu_assert("v2 alloc failed", !err);
    mu_assert("v2 ref count is 1 before demotion", vmaf_ref_load(v2.ref) == 1);

    err = vmaf_picture_v2_to_v1(&v2, &v1);
    mu_assert("v2_to_v1 failed", !err);

    char *msg = check_v2_to_v1_fields(&v2, &v1);
    if (msg)
        return msg;

    err = vmaf_picture_unref(&v1);
    mu_assert("v1 unref failed", !err);
    mu_assert("ref count back to 1 after v1 unref", vmaf_ref_load(v2.ref) == 1);
    err = vmaf_picture2_unref(&v2);
    mu_assert("v2 unref failed", !err);
    return NULL;
}

static char *test_v2_to_v1_null_args(void)
{
    VmafPicture2 v2;
    VmafPicture v1;
    int err;

    memset(&v2, 0, sizeof(v2));
    memset(&v1, 0, sizeof(v1));

    err = vmaf_picture_v2_to_v1(NULL, &v1);
    mu_assert("NULL src must return -EINVAL", err == -EINVAL);

    err = vmaf_picture_v2_to_v1(&v2, NULL);
    mu_assert("NULL dst must return -EINVAL", err == -EINVAL);

    /* Zeroed v2 (ref == NULL) must be rejected. */
    err = vmaf_picture_v2_to_v1(&v2, &v1);
    mu_assert("zeroed v2 (no ref) must return -EINVAL", err == -EINVAL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Round-trip: v1 → v2 → v1                                           */
/* ------------------------------------------------------------------ */

/* Unref all three round-trip handles, split out of test_v1_v2_roundtrip for
 * the same branch-budget reason as check_picture2_alloc_fields above. */
static char *check_roundtrip_cleanup(VmafPicture *copy, VmafPicture2 *mid, VmafPicture *orig)
{
    int err = vmaf_picture_unref(copy);
    mu_assert("copy unref failed", !err);
    err = vmaf_picture2_unref(mid);
    mu_assert("mid unref failed", !err);
    err = vmaf_picture_unref(orig);
    mu_assert("orig unref failed", !err);
    return NULL;
}

static char *test_v1_v2_roundtrip(void)
{
    int err;
    VmafPicture orig;
    VmafPicture2 mid;
    VmafPicture copy;

    err = vmaf_picture_alloc(&orig, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    mu_assert("orig alloc failed", !err);

    /* Fill luma plane with a known pattern. */
    memset(orig.data[0], 0xAB, (size_t)(orig.stride[0] * (ptrdiff_t)orig.h[0]));

    err = vmaf_picture_v1_to_v2(&orig, &mid);
    mu_assert("v1→v2 failed", !err);

    err = vmaf_picture_v2_to_v1(&mid, &copy);
    mu_assert("v2→v1 failed", !err);

    /* All three share the same backing buffer. */
    mu_assert("copy.data[0] == orig.data[0]", copy.data[0] == orig.data[0]);
    mu_assert("ref count is 3 with all three alive", vmaf_ref_load(orig.ref) == 3);

    /* Verify the fill pattern survived the round-trip (pointer sharing). */
    mu_assert("luma data matches after round-trip", ((uint8_t *)copy.data[0])[0] == 0xAB);

    char *msg = check_roundtrip_cleanup(&copy, &mid, &orig);
    if (msg)
        return msg;
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_backend_handle_name_known_values),
        MU_TEST(test_backend_handle_name_out_of_range),
        MU_TEST(test_picture2_alloc_and_unref),
        MU_TEST(test_picture2_alloc_null_rejected),
        MU_TEST(test_picture2_unref_null_noop),
        MU_TEST(test_v1_to_v2_basic),
        MU_TEST(test_v1_to_v2_null_args),
        MU_TEST(test_v2_to_v1_basic),
        MU_TEST(test_v2_to_v1_null_args),
        MU_TEST(test_v1_v2_roundtrip),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
