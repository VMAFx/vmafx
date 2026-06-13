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

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if HAVE_SYCL

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

static VmafSyclState *sycl = NULL;
static int sycl_init_failed = 0;

static char *test_sycl_pool_init_sycl(void)
{
    VmafSyclConfiguration cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl, cfg);
    if (err) {
        fprintf(stderr, "  [SKIP] SYCL state init failed (err=%d), no GPU\n", err);
        sycl_init_failed = 1;
        sycl = NULL;
    }
    return NULL;
}

static char *test_sycl_preallocate_none(void)
{
    if (sycl_init_failed) {
        fprintf(stderr, "  [SKIP] test_sycl_preallocate_none (no GPU)\n");
        return NULL;
    }

    VmafConfiguration vmaf_cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 1};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("vmaf_init should succeed", err == 0);

    err = vmaf_sycl_import_state(vmaf, sycl);
    mu_assert("vmaf_sycl_import_state should succeed", err == 0);

    VmafSyclPictureConfiguration pic_cfg = {
        .pic_params = {.w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
        .pic_prealloc_method = VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE,
    };
    err = vmaf_sycl_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("preallocate with NONE should succeed as no-op", err == 0);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_sycl_preallocate_device_fetch_cycle(void)
{
    if (sycl_init_failed) {
        fprintf(stderr, "  [SKIP] test_sycl_preallocate_device_fetch_cycle (no GPU)\n");
        return NULL;
    }

    VmafConfiguration vmaf_cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 1};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("vmaf_init should succeed", err == 0);

    err = vmaf_sycl_import_state(vmaf, sycl);
    mu_assert("vmaf_sycl_import_state should succeed", err == 0);

    VmafSyclPictureConfiguration pic_cfg = {
        .pic_params = {.w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
        .pic_prealloc_method = VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE,
    };
    err = vmaf_sycl_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("preallocate DEVICE should succeed", err == 0);

    /* Repeat alloc-fails: second preallocate call should reject */
    err = vmaf_sycl_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("second preallocate should fail with EBUSY", err != 0);

    /* Exercise fetch → unref cycle across the ring; a pool depth of 2 means
     * we must be able to cycle through >N frames. */
    for (unsigned i = 0; i < 10; i++) {
        VmafPicture pic;
        memset(&pic, 0, sizeof(pic));
        err = vmaf_sycl_picture_fetch(vmaf, &pic);
        mu_assert("picture_fetch should succeed", err == 0);
        mu_assert("fetched picture should have Y-plane data", pic.data[0] != NULL);
        mu_assert("fetched picture should have ref", pic.ref != NULL);
        mu_assert("picture width preserved", pic.w[0] == 1920);
        mu_assert("picture height preserved", pic.h[0] == 1080);
        mu_assert("picture bpc preserved", pic.bpc == 8);
        err = vmaf_picture_unref(&pic);
        mu_assert("picture_unref should succeed", err == 0);
    }

    /* vmaf_close should release the pool without leaking. */
    err = vmaf_close(vmaf);
    mu_assert("vmaf_close should succeed with pool", err == 0);
    return NULL;
}

static char *test_sycl_preallocate_host_fetch_cycle(void)
{
    if (sycl_init_failed) {
        fprintf(stderr, "  [SKIP] test_sycl_preallocate_host_fetch_cycle (no GPU)\n");
        return NULL;
    }

    VmafConfiguration vmaf_cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 1};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("vmaf_init should succeed", err == 0);

    err = vmaf_sycl_import_state(vmaf, sycl);
    mu_assert("vmaf_sycl_import_state should succeed", err == 0);

    VmafSyclPictureConfiguration pic_cfg = {
        .pic_params = {.w = 640, .h = 480, .bpc = 10, .pix_fmt = VMAF_PIX_FMT_YUV420P},
        .pic_prealloc_method = VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST,
    };
    err = vmaf_sycl_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("preallocate HOST should succeed", err == 0);

    /* Host USM is CPU-writable — sanity-check a write+read round-trip. */
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    err = vmaf_sycl_picture_fetch(vmaf, &pic);
    mu_assert("picture_fetch (host) should succeed", err == 0);
    mu_assert("host-pool picture has data[0]", pic.data[0] != NULL);

    /* Write + read back a sentinel byte at start and end of the buffer. */
    uint8_t *y = (uint8_t *)pic.data[0];
    y[0] = 0x5A;
    y[1] = 0xA5;
    mu_assert("host buffer byte 0 round-trips", y[0] == 0x5A);
    mu_assert("host buffer byte 1 round-trips", y[1] == 0xA5);

    err = vmaf_picture_unref(&pic);
    mu_assert("picture_unref should succeed", err == 0);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close should succeed with host pool", err == 0);
    return NULL;
}

static char *test_sycl_preallocate_without_state(void)
{
    /* preallocate_pictures without an imported state must fail. */
    VmafConfiguration vmaf_cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 1};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("vmaf_init should succeed", err == 0);

    VmafSyclPictureConfiguration pic_cfg = {
        .pic_params = {.w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
        .pic_prealloc_method = VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE,
    };
    err = vmaf_sycl_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("preallocate without state should fail", err != 0);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_sycl_pool_release(void)
{
    if (sycl_init_failed || sycl == NULL) {
        fprintf(stderr, "  [SKIP] test_sycl_pool_release (no GPU)\n");
        return NULL;
    }
    vmaf_sycl_state_free(&sycl);
    mu_assert("state should be NULL after free", sycl == NULL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_sycl_preallocate_without_state);
    mu_run_test(test_sycl_pool_init_sycl);
    mu_run_test(test_sycl_preallocate_none);
    mu_run_test(test_sycl_preallocate_device_fetch_cycle);
    mu_run_test(test_sycl_preallocate_host_fetch_cycle);
    mu_run_test(test_sycl_pool_release);
    return NULL;
}

#else /* !HAVE_SYCL */

char *run_tests(void)
{
    (void)fprintf(stderr, "SYCL not enabled, skipping tests\n");
    return NULL;
}

#endif /* HAVE_SYCL */
