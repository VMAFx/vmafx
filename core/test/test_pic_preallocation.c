/**
 *
 *  Copyright 2016-2025 Netflix, Inc.
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

#include "test.h"
#include "mu_table.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static void sleep_microseconds(unsigned int microseconds)
{
#ifdef _WIN32
    Sleep((microseconds + 999U) / 1000U);
#else
    const struct timespec duration = {
        .tv_sec = microseconds / 1000000U,
        .tv_nsec = (long)(microseconds % 1000000U) * 1000L,
    };
    (void)nanosleep(&duration, VMAF_NULLPTR);
#endif
}

static int run_preallocated_model(unsigned n_threads, VmafPictureConfiguration pic_cfg,
                                  unsigned frame_count)
{
    VmafContext *vmaf = VMAF_NULLPTR;
    VmafModel *model = VMAF_NULLPTR;
    const VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = n_threads,
    };
    int err = vmaf_init(&vmaf, vmaf_cfg);
    if (err)
        return err;
    err = vmaf_preallocate_pictures(vmaf, pic_cfg);
    if (err)
        goto cleanup;

    VmafModelConfig model_cfg = {VMAF_NULLPTR};
    err = vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1");
    if (err)
        goto cleanup;
    err = vmaf_use_features_from_model(vmaf, model);
    if (err)
        goto cleanup;

    for (unsigned i = 0; i < frame_count; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = vmaf_fetch_preallocated_picture(vmaf, &ref);
        if (err)
            goto cleanup;
        err = vmaf_fetch_preallocated_picture(vmaf, &dist);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            goto cleanup;
        }
        if (ref.pix_fmt != pic_cfg.pic_params.pix_fmt ||
            dist.pix_fmt != pic_cfg.pic_params.pix_fmt) {
            (void)vmaf_picture_unref(&ref);
            (void)vmaf_picture_unref(&dist);
            err = -EINVAL;
            goto cleanup;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            goto cleanup;
    }
    err = vmaf_read_pictures(vmaf, VMAF_NULLPTR, VMAF_NULLPTR, 0);

cleanup:
    vmaf_model_destroy(model);
    const int close_err = vmaf_close(vmaf);
    return err ? err : close_err;
}

static char *test_picture_pool_basic()
{
    const VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 1920,
                .h = 1080,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 40,
    };
    mu_assert("large preallocated model run failed", run_preallocated_model(4, pic_cfg, 10) == 0);
    return VMAF_NULLPTR;
}

static char *test_picture_pool_small()
{
    const VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 640,
                .h = 480,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 8,
    };
    mu_assert("small preallocated model run failed", run_preallocated_model(2, pic_cfg, 3) == 0);
    return VMAF_NULLPTR;
}

static char *test_picture_pool_fetch_unref_cycle()
{
    int err = 0;

    VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = 4,
    };

    VmafContext *vmaf = VMAF_NULLPTR;
    err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("problem during vmaf_init", !err);

    VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 1920,
                .h = 1080,
                .bpc = 10,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 16,
    };

    err = vmaf_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("problem during vmaf_preallocate_pictures", !err);

    // Test multiple fetch/unref cycles without vmaf_read_pictures
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        VmafPicture pics[4];

        // Fetch pictures
        for (unsigned i = 0; i < 4; i++) {
            err = vmaf_fetch_preallocated_picture(vmaf, &pics[i]);
            mu_assert("problem during vmaf_fetch_preallocated_picture", !err);
        }

        // Unref all pictures
        for (unsigned i = 0; i < 4; i++) {
            err = vmaf_picture_unref(&pics[i]);
            mu_assert("problem during vmaf_picture_unref", !err);
        }
    }

    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);

    return VMAF_NULLPTR;
}

static char *test_picture_pool_yuv444()
{
    const VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 1920,
                .h = 1080,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV444P,
            },
        .pic_cnt = 20,
    };
    mu_assert("YUV444 preallocated model run failed", run_preallocated_model(4, pic_cfg, 5) == 0);
    return VMAF_NULLPTR;
}

static int exercise_picture_reuse(VmafContext *vmaf)
{
    VmafPicture pics[3];
    int err = vmaf_fetch_preallocated_picture(vmaf, &pics[0]);
    if (err)
        return err;
    const void *const first_data_ptr = pics[0].data[0];
    err = vmaf_fetch_preallocated_picture(vmaf, &pics[1]);
    if (err) {
        (void)vmaf_picture_unref(&pics[0]);
        return err;
    }
    err = vmaf_picture_unref(&pics[0]);
    if (err) {
        (void)vmaf_picture_unref(&pics[1]);
        return err;
    }
    err = vmaf_fetch_preallocated_picture(vmaf, &pics[2]);
    if (err) {
        (void)vmaf_picture_unref(&pics[1]);
        return err;
    }
    if (pics[2].data[0] != first_data_ptr)
        err = -EIO;
    const int first_unref_err = vmaf_picture_unref(&pics[1]);
    const int second_unref_err = vmaf_picture_unref(&pics[2]);
    if (!err)
        err = first_unref_err ? first_unref_err : second_unref_err;
    return err;
}

// Test pool exhaustion and blocking behavior
static char *test_picture_pool_exhaustion()
{
    const VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = 4,
    };
    VmafContext *vmaf = VMAF_NULLPTR;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("problem during vmaf_init", !err);

    const VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 640,
                .h = 480,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 2,
    };
    err = vmaf_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("problem during vmaf_preallocate_pictures", !err);
    mu_assert("picture reuse cycle failed", exercise_picture_reuse(vmaf) == 0);
    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);
    return VMAF_NULLPTR;
}

// Multi-threaded test data
typedef struct {
    VmafContext *vmaf;
    int fetch_count;
    int error;
} thread_test_data;

static void *thread_fetch_worker(void *arg)
{
    thread_test_data *data = (thread_test_data *)arg;

    for (int i = 0; i < data->fetch_count; i++) {
        VmafPicture pic;
        int err = vmaf_fetch_preallocated_picture(data->vmaf, &pic);
        if (err) {
            data->error = err;
            return VMAF_NULLPTR;
        }

        // Simulate some work
        sleep_microseconds(100); // 0.1ms

        err = vmaf_picture_unref(&pic);
        if (err) {
            data->error = err;
            return VMAF_NULLPTR;
        }
    }

    return VMAF_NULLPTR;
}

// Test concurrent access from multiple threads
static char *test_picture_pool_multithreaded()
{
    int err = 0;

    VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = 8,
    };

    VmafContext *vmaf = VMAF_NULLPTR;
    err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("problem during vmaf_init", !err);

    VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 1920,
                .h = 1080,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 8, // Small pool to stress test
    };

    err = vmaf_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("problem during vmaf_preallocate_pictures", !err);

    enum { num_threads = 4, fetches_per_thread = 20 };
    pthread_t threads[num_threads];
    thread_test_data thread_data[num_threads];

    // Start threads
    int threads_created = 0;
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].vmaf = vmaf;
        thread_data[i].fetch_count = fetches_per_thread;
        thread_data[i].error = 0;

        err = pthread_create(&threads[i], VMAF_NULLPTR, thread_fetch_worker, &thread_data[i]);
        if (err) {
            mu_assert("problem creating thread", 0);
        }
        threads_created++;
    }

    // Wait for all threads
    int thread_err = 0;
    for (int i = 0; i < threads_created; i++) {
        pthread_join(threads[i], VMAF_NULLPTR);
        if (thread_data[i].error != 0)
            thread_err = thread_data[i].error;
    }
    mu_assert("thread encountered error", thread_err == 0);

    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);

    return VMAF_NULLPTR;
}

// Test that close waits for all pictures to be returned
static void *thread_delayed_unref(void *arg)
{
    VmafPicture *pic = (VmafPicture *)arg;

    // Hold picture for a while
    sleep_microseconds(1000000U);

    // Then return it
    vmaf_picture_unref(pic);

    return VMAF_NULLPTR;
}

static char *test_picture_pool_close_waits()
{
    int err = 0;

    VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = 2,
    };

    VmafContext *vmaf = VMAF_NULLPTR;
    err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("problem during vmaf_init", !err);

    VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 640,
                .h = 480,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 4,
    };

    err = vmaf_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("problem during vmaf_preallocate_pictures", !err);

    // Fetch a picture
    VmafPicture pic;
    err = vmaf_fetch_preallocated_picture(vmaf, &pic);
    mu_assert("problem during vmaf_fetch_preallocated_picture", !err);

    // Start thread that will hold picture for 1 second then return it
    pthread_t thread;
    err = pthread_create(&thread, VMAF_NULLPTR, thread_delayed_unref, &pic);
    mu_assert("problem creating thread", !err);

    // Try to close - should block until thread returns picture
    // This will take ~1 second
    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);

    pthread_join(thread, VMAF_NULLPTR);

    return VMAF_NULLPTR;
}

// Stress test with high contention
static char *test_picture_pool_stress()
{
    int err = 0;

    VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_WARNING,
        .n_threads = 16,
    };

    VmafContext *vmaf = VMAF_NULLPTR;
    err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("problem during vmaf_init", !err);

    // Very small pool relative to thread count
    VmafPictureConfiguration pic_cfg = {
        .pic_params =
            {
                .w = 640,
                .h = 480,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = 4, // Only 4 pictures for 16 threads!
    };

    err = vmaf_preallocate_pictures(vmaf, pic_cfg);
    mu_assert("problem during vmaf_preallocate_pictures", !err);

    enum { num_threads = 16, fetches_per_thread = 50 };
    pthread_t threads[num_threads];
    thread_test_data thread_data[num_threads];

    // Start threads - high contention for limited pool
    int threads_created_s = 0;
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].vmaf = vmaf;
        thread_data[i].fetch_count = fetches_per_thread;
        thread_data[i].error = 0;

        err = pthread_create(&threads[i], VMAF_NULLPTR, thread_fetch_worker, &thread_data[i]);
        if (err) {
            mu_assert("problem creating thread", 0);
        }
        threads_created_s++;
    }

    // Wait for all threads
    int thread_err_s = 0;
    for (int i = 0; i < threads_created_s; i++) {
        pthread_join(threads[i], VMAF_NULLPTR);
        if (thread_data[i].error != 0)
            thread_err_s = thread_data[i].error;
    }
    mu_assert("thread encountered error", thread_err_s == 0);

    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);

    return VMAF_NULLPTR;
}

char *run_tests()
{
    static const MuTest tests[] = {
        MU_TEST(test_picture_pool_basic),
        MU_TEST(test_picture_pool_small),
        MU_TEST(test_picture_pool_fetch_unref_cycle),
        MU_TEST(test_picture_pool_yuv444),
        MU_TEST(test_picture_pool_exhaustion),
        MU_TEST(test_picture_pool_multithreaded),
        MU_TEST(test_picture_pool_close_waits),
        MU_TEST(test_picture_pool_stress),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}
