/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
#include "mu_table.h"
#include "picture.h"
#include "libvmaf/picture.h"
#include "ref.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) — explicitly walks every alloc / fill / ref / unref state to keep failures localised; splitting hides the assertion that fired.
static char *test_picture_alloc_ref_and_unref(void)
{
    int err;

    VmafPicture pic_a;
    VmafPicture pic_b;
    err = vmaf_picture_alloc(&pic_a, VMAF_PIX_FMT_YUV420P, 8, 1920, 1080);
    mu_assert("problem during vmaf_picture_alloc", !err);
    mu_assert("pic_a.ref->cnt should be 1", vmaf_ref_load(pic_a.ref) == 1);
    err = vmaf_picture_ref(&pic_b, &pic_a);
    mu_assert("problem during vmaf_picture_ref", !err);
    mu_assert("pic_a.ref->cnt should be 2", vmaf_ref_load(pic_a.ref) == 2);
    mu_assert("pic_b.ref->cnt should be 2", vmaf_ref_load(pic_b.ref) == 2);
    err = vmaf_picture_unref(&pic_a);
    mu_assert("problem during vmaf_picture_unref", !err);
    mu_assert("pic_b.ref->cnt should be 1", vmaf_ref_load(pic_b.ref) == 1);
    err = vmaf_picture_unref(&pic_b);
    mu_assert("problem during vmaf_picture_unref", !err);

    return NULL;
}

static char *test_picture_data_alignment(void)
{
    int err;

    VmafPicture pic;
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 10, 1920 + 1, 1080);
    mu_assert("problem during vmaf_picture_alloc", !err);
    mu_assert("picture data is not 32-byte alligned",
              !(((uintptr_t)pic.data[0]) % 32) && !(((uintptr_t)pic.data[1]) % 32) &&
                  !(((uintptr_t)pic.data[2]) % 32) && !(pic.stride[0] % 32) &&
                  !(pic.stride[1] % 32) && !(pic.stride[2] % 32));
    err = vmaf_picture_unref(&pic);
    mu_assert("problem during vmaf_picture_unref", !err);

    return NULL;
}

/*
 * Regression test for Research-0094: odd-height / odd-width YUV 4:2:0 inputs
 * must produce ceil(luma/2) chroma rows/columns, not floor.  Pre-fix,
 * picture_compute_geometry used plain right-shift (floor), which under-
 * allocated chroma planes by one row for any input with an odd luma dimension.
 * Consumers such as ciede::scale_chroma_planes would then walk one row past
 * the allocation, causing an ASan-detected heap OOB.
 *
 * Canonical reproducer: 577x323 YUV 4:2:0 (both dimensions odd).
 *   luma:   w=577, h=323
 *   chroma: w=ceil(577/2)=289, h=ceil(323/2)=162   (correct, post-fix)
 *           w=floor(577/2)=288, h=floor(323/2)=161  (wrong, pre-fix)
 */
/* One case per pixel format. The combined test tripped the
 * readability-function-size branch budget (ADR-1142) -- every mu_assert is a
 * branch -- and each format is an independent assertion anyway, so a failure
 * now names the format it came from. */
static char *test_picture_chroma_ceiling_420_odd(void)
{
    VmafPicture pic;
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 577, 323);
    mu_assert("vmaf_picture_alloc failed for 577x323 YUV420", !err);
    mu_assert("chroma w must be ceil(577/2)=289 for odd-width 4:2:0", pic.w[1] == 289);
    mu_assert("chroma w[2] must equal w[1]", pic.w[2] == pic.w[1]);
    mu_assert("chroma h must be ceil(323/2)=162 for odd-height 4:2:0", pic.h[1] == 162);
    mu_assert("chroma h[2] must equal h[1]", pic.h[2] == pic.h[1]);
    err = vmaf_picture_unref(&pic);
    mu_assert("vmaf_picture_unref failed", !err);
    return NULL;
}

static char *test_picture_chroma_ceiling_420_even(void)
{
    VmafPicture pic;
    /* Even dims — the ceiling must equal the floor (no change). */
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 576, 324);
    mu_assert("vmaf_picture_alloc failed for 576x324 YUV420", !err);
    mu_assert("chroma w must be 288 for even-width 4:2:0", pic.w[1] == 288);
    mu_assert("chroma h must be 162 for even-height 4:2:0", pic.h[1] == 162);
    err = vmaf_picture_unref(&pic);
    mu_assert("vmaf_picture_unref failed", !err);
    return NULL;
}

static char *test_picture_chroma_ceiling_422(void)
{
    VmafPicture pic;
    /* Odd width only — height must stay the full luma height. */
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV422P, 8, 577, 323);
    mu_assert("vmaf_picture_alloc failed for 577x323 YUV422", !err);
    mu_assert("chroma w must be ceil(577/2)=289 for odd-width 4:2:2", pic.w[1] == 289);
    mu_assert("chroma h must equal luma h for 4:2:2", pic.h[1] == 323);
    err = vmaf_picture_unref(&pic);
    mu_assert("vmaf_picture_unref failed", !err);
    return NULL;
}

static char *test_picture_chroma_ceiling_444(void)
{
    VmafPicture pic;
    /* No subsampling — chroma dims equal luma dims. */
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV444P, 8, 577, 323);
    mu_assert("vmaf_picture_alloc failed for 577x323 YUV444", !err);
    mu_assert("chroma w must equal luma w for 4:4:4", pic.w[1] == 577);
    mu_assert("chroma h must equal luma h for 4:4:4", pic.h[1] == 323);
    err = vmaf_picture_unref(&pic);
    mu_assert("vmaf_picture_unref failed", !err);
    return NULL;
}

/*
 * Regression test for audit finding #10: integer overflow in
 * picture_compute_geometry when caller passes near-UINT_MAX width.
 * Before the fix, (w + DATA_ALIGN - 1u) wrapped to 0, producing a
 * zero-byte allocation that passed silently and caused OOB on any pixel
 * read.  After the fix, vmaf_picture_alloc rejects w > 32768 or h > 32768
 * with -EINVAL before any arithmetic.  CERT INT30-C.
 */
static char *test_picture_alloc_rejects_overflow_dimensions(void)
{
    int err;
    VmafPicture pic;

    /* w == 0 must be rejected. */
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 0, 1080);
    mu_assert("vmaf_picture_alloc must reject w=0 with -EINVAL", err == -EINVAL);

    /* h == 0 must be rejected. */
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 0);
    mu_assert("vmaf_picture_alloc must reject h=0 with -EINVAL", err == -EINVAL);

    /* w > 32768 must be rejected. */
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 32769, 1080);
    mu_assert("vmaf_picture_alloc must reject w=32769 with -EINVAL", err == -EINVAL);

    /* h > 32768 must be rejected. */
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 32769);
    mu_assert("vmaf_picture_alloc must reject h=32769 with -EINVAL", err == -EINVAL);

    /* Boundary: w==32768, h==32768 must succeed. */
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 32768, 32768);
    mu_assert("vmaf_picture_alloc must accept w=32768, h=32768", !err);
    err = vmaf_picture_unref(&pic);
    mu_assert("vmaf_picture_unref failed for 32768x32768", !err);

    return NULL;
}

static char *check_picture_alloc_yuv400p_luma_only(unsigned w, unsigned h)
{
    VmafPicture pic;
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV400P, 8, w, h);
    mu_assert("vmaf_picture_alloc failed for YUV400P", !err);

    const int luma_only = pic.data[0] != NULL && pic.w[0] == w && pic.h[0] == h;
    const int no_chroma = pic.data[1] == NULL && pic.data[2] == NULL && pic.w[1] == 0 &&
                          pic.w[2] == 0 && pic.h[1] == 0 && pic.h[2] == 0;

    err = vmaf_picture_unref(&pic);
    mu_assert("vmaf_picture_unref failed for YUV400P", !err);
    mu_assert("YUV400P luma plane must match the requested geometry", luma_only);
    mu_assert("YUV400P chroma planes must stay unallocated with zero geometry", no_chroma);
    return NULL;
}

/* VMAF_PIX_FMT_YUV400P is luma-only for both ordinary and odd dimensions.
 * Keep this at the public picture-allocation seam: extractor tests that happen
 * to consume YUV400P do not pin the zeroed chroma geometry contract. */
static char *test_picture_alloc_yuv400p_luma_only(void)
{
    char *msg = check_picture_alloc_yuv400p_luma_only(1920, 1080);
    if (msg)
        return msg;
    return check_picture_alloc_yuv400p_luma_only(577, 323);
}

/*
 * Error-path coverage for vmaf_picture_ref and vmaf_picture_unref.
 *
 * vmaf_picture_ref:
 *   - NULL dst → must return -EINVAL without touching *src.
 *   - NULL src → must return -EINVAL without touching *dst.
 *
 * vmaf_picture_unref:
 *   - NULL pic → must return -EINVAL.
 *   - zeroed pic (pic->ref == NULL) → must return -EINVAL.
 *
 * These paths were added as part of the 2026-06 error-path coverage
 * audit (r12) triggered by PR #765 (ADR-1072) and PR #766 (ADR-1073).
 */
static char *test_picture_ref_null_error_paths(void)
{
    int err;
    VmafPicture src;
    VmafPicture dst;

    err = vmaf_picture_alloc(&src, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    mu_assert("setup: vmaf_picture_alloc failed", !err);

    /* NULL dst: ref count on src must not change. */
    err = vmaf_picture_ref(NULL, &src);
    mu_assert("vmaf_picture_ref(NULL, src) must return -EINVAL", err == -EINVAL);

    /* NULL src: dst must remain untouched (unmodified zero-init). */
    memset(&dst, 0, sizeof(dst));
    err = vmaf_picture_ref(&dst, NULL);
    mu_assert("vmaf_picture_ref(dst, NULL) must return -EINVAL", err == -EINVAL);
    mu_assert("vmaf_picture_ref(dst, NULL) must not write to dst", dst.ref == NULL);

    err = vmaf_picture_unref(&src);
    mu_assert("vmaf_picture_unref (cleanup) failed", !err);

    return NULL;
}

static char *test_picture_unref_null_error_paths(void)
{
    int err;

    /* NULL pic pointer → must return -EINVAL without crashing. */
    err = vmaf_picture_unref(NULL);
    mu_assert("vmaf_picture_unref(NULL) must return -EINVAL", err == -EINVAL);

    /* Zeroed VmafPicture (pic->ref == NULL) → must return -EINVAL.
     * This covers the second guard in vmaf_picture_unref and mirrors the
     * real scenario triggered when prev_ref is memset'd to zero before
     * an unref call (ADR-1072 fix pattern). */
    VmafPicture zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    err = vmaf_picture_unref(&zeroed);
    mu_assert("vmaf_picture_unref(zeroed pic) must return -EINVAL", err == -EINVAL);

    return NULL;
}

/* vmaf_picture_alloc's documented layout (core/include/libvmaf/picture.h): each
 * plane's stride is its width rounded up to 64 samples (bytes for 8-bit, twice
 * that above 8 bits) and the buffer is zero-filled. The header once said "32-byte
 * boundary" and "uninitialised"; a caller trusting that wrote the wrong padding. */
static char *test_picture_stride_is_64_samples(void)
{
    VmafPicture pic;
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 577, 323);
    mu_assert("vmaf_picture_alloc failed for 577x323 8-bit", !err);
    mu_assert("8-bit luma stride must be 640 bytes (577 rounded up to 64)", pic.stride[0] == 640);
    mu_assert("8-bit chroma stride must be 320 bytes (289 rounded up to 64)",
              pic.stride[1] == 320 && pic.stride[2] == 320);
    err = vmaf_picture_unref(&pic);
    mu_assert("problem during vmaf_picture_unref", !err);

    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 10, 577, 323);
    mu_assert("vmaf_picture_alloc failed for 577x323 10-bit", !err);
    mu_assert("10-bit luma stride must be 1280 bytes (640 samples of two bytes)",
              pic.stride[0] == 1280);
    err = vmaf_picture_unref(&pic);
    mu_assert("problem during vmaf_picture_unref", !err);
    return NULL;
}

static char *test_picture_buffer_is_zero_filled(void)
{
    VmafPicture pic;
    int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 577, 323);
    mu_assert("vmaf_picture_alloc failed for 577x323 8-bit", !err);
    const uint8_t *luma = (const uint8_t *)pic.data[0];
    size_t nonzero = 0;
    for (size_t i = 0; i < (size_t)pic.stride[0] * pic.h[0]; i++)
        nonzero += luma[i] != 0;
    mu_assert("a freshly allocated luma plane must be zero-filled", nonzero == 0);
    err = vmaf_picture_unref(&pic);
    mu_assert("problem during vmaf_picture_unref", !err);
    return NULL;
}

/* A table, not a sequence of mu_run_test(): every mu_run_test expands to two
 * branches and readability-function-size allows 15 (mu_table.h, ADR-1142). */
char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_picture_alloc_ref_and_unref),
        MU_TEST(test_picture_data_alignment),
        MU_TEST(test_picture_chroma_ceiling_420_odd),
        MU_TEST(test_picture_chroma_ceiling_420_even),
        MU_TEST(test_picture_chroma_ceiling_422),
        MU_TEST(test_picture_chroma_ceiling_444),
        MU_TEST(test_picture_stride_is_64_samples),
        MU_TEST(test_picture_buffer_is_zero_filled),
        MU_TEST(test_picture_alloc_rejects_overflow_dimensions),
        MU_TEST(test_picture_alloc_yuv400p_luma_only),
        MU_TEST(test_picture_ref_null_error_paths),
        MU_TEST(test_picture_unref_null_error_paths),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
