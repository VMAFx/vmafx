/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/**
 * @file test_picture_convert_api.c
 *
 * ADR-1822: the additive `vmaf_picture_convert*` API.
 *
 *  - Layout guard: adding the colour types must not change `VmafPicture`
 *    (HISS-14 append-only ABI). Upstream's 0497a0f29 inserted a `VmafColor`
 *    before `ref`; here the members stay in the order and at the offsets a
 *    consumer built against the old header expects. The checks are relative
 *    (each member follows the previous one), so they hold on every ABI.
 *  - Without zimg (the default build) every entry point fails with -ENOTSUP,
 *    writes nothing and logs why.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "libvmaf/picture.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* The layout of VmafPicture before 0497a0f29, member after member. */
_Static_assert(offsetof(VmafPicture, bpc) >= sizeof(enum VmafPixelFormat), "bpc follows pix_fmt");
_Static_assert(offsetof(VmafPicture, h) == offsetof(VmafPicture, w) + 3 * sizeof(unsigned),
               "h[3] follows w[3]");
_Static_assert(offsetof(VmafPicture, stride) >= offsetof(VmafPicture, h) + 3 * sizeof(unsigned),
               "stride[3] follows h[3]");
_Static_assert(offsetof(VmafPicture, data) == offsetof(VmafPicture, stride) + 3 * sizeof(ptrdiff_t),
               "data[3] follows stride[3]");
_Static_assert(offsetof(VmafPicture, ref) == offsetof(VmafPicture, data) + 3 * sizeof(void *),
               "ref directly follows data[3]: no colour field before it");
_Static_assert(offsetof(VmafPicture, priv) == offsetof(VmafPicture, ref) + sizeof(void *),
               "priv follows ref");
_Static_assert(sizeof(VmafPicture) == offsetof(VmafPicture, priv) + sizeof(void *),
               "priv is the last member: no trailing colour field");

static char *test_picture_layout_is_unchanged(void)
{
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    mu_assert("zeroed picture has no ref", pic.ref == NULL);
    mu_assert("VmafColor is four enums", sizeof(VmafColor) == 4 * sizeof(int));
    return NULL;
}

#ifndef HAVE_ZIMG
static char *test_picture_convert_without_zimg(void)
{
    VmafPicture src;
    VmafPicture dst;
    int err = vmaf_picture_alloc(&src, VMAF_PIX_FMT_YUV420P, 8, 16, 16);
    mu_assert("problem during vmaf_picture_alloc", !err);
    memset(&dst, 0, sizeof(dst));

    VmafColor color = {
        .range = VMAF_COLOR_RANGE_LIMITED,
        .primaries = VMAF_COLOR_PRIMARIES_BT709,
        .trc = VMAF_COLOR_TRC_BT709,
        .matrix = VMAF_COLOR_MATRIX_BT709,
    };
    VmafPictureConvertTarget target = {
        .pix_fmt = VMAF_PIX_FMT_YUV444P,
        .bpc = 8,
        .color = color,
    };
    VmafPictureConvertContext *ctx = NULL;
    err = vmaf_picture_convert_context_init_with_color(&ctx, &src, &color, &target);
    mu_assert("init should be unsupported without zimg", err == -ENOTSUP);
    mu_assert("no context should be created without zimg", !ctx);

    err = vmaf_picture_convert(ctx, &dst, &src);
    mu_assert("convert should be unsupported without zimg", err == -ENOTSUP);
    mu_assert("dst should not be allocated without zimg", !dst.ref);

    err = vmaf_picture_convert_context_close(ctx);
    mu_assert("close should be unsupported without zimg", err == -ENOTSUP);

    err = vmaf_picture_unref(&src);
    mu_assert("problem during vmaf_picture_unref", !err);
    return NULL;
}
#endif

static const MuTest base_tests[] = {
    MU_TEST(test_picture_layout_is_unchanged),
};

#ifndef HAVE_ZIMG
static const MuTest no_zimg_tests[] = {
    MU_TEST(test_picture_convert_without_zimg),
};
#endif

/* Run picture conversion API tests and return the first failure message, or NULL on success. */
char *run_tests(void)
{
    char *msg = mu_run_table(base_tests, MU_TABLE_LEN(base_tests));
    if (msg)
        return msg;
#ifndef HAVE_ZIMG
    return mu_run_table(no_zimg_tests, MU_TABLE_LEN(no_zimg_tests));
#else
    return NULL;
#endif
}

/* NOLINTEND(modernize-use-nullptr) */
