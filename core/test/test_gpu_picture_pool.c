/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define usleep(us) Sleep(((us) + 999) / 1000)
#define sleep(s) Sleep((s) * 1000)
#else
#include <unistd.h>
#endif

#include "test.h"

#include "libvmaf/libvmaf_cuda.h"

#include "cuda/common.h"
#include "cuda/picture_cuda.h"
#include "gpu_picture_pool.h"
#include "thread_pool.h"

static char *test_ring_buffer()
{
    VmafCudaCookie my_cookie = {
        .w = 1920,
        .h = 1080,
        .bpc = 8,
        .pix_fmt = VMAF_PIX_FMT_YUV400P,
    };

    VmafCudaConfiguration cu_cfg = {0};
    int err = vmaf_cuda_state_init(&my_cookie.state, cu_cfg);
    if (err || !my_cookie.state) {
        free(my_cookie.state);
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 4,
        .cookie = &my_cookie,
        .alloc_picture_callback = vmaf_cuda_picture_alloc,
        .free_picture_callback = vmaf_cuda_picture_free,
        .synchronize_picture_callback = vmaf_cuda_picture_synchronize,
    };

    VmafGpuPicturePool *ring_buffer;
    err = vmaf_gpu_picture_pool_init(&ring_buffer, cfg);
    mu_assert("problem during vmaf_picture_pool_init", !err);

    VmafPicture pic_1;
    err = vmaf_gpu_picture_pool_fetch(ring_buffer, &pic_1);
    mu_assert("problem during vmaf_picture_pool_request_picture", !err);
    mu_assert("data[0] should have been allocated", pic_1.data[0]);
    mu_assert("data[1] should not have been allocated", !pic_1.data[1]);
    mu_assert("data[2] should not have been allocated", !pic_1.data[2]);
    mu_assert("pix_fmt should be VMAF_PIX_FMT_YUV400P", pic_1.pix_fmt == VMAF_PIX_FMT_YUV400P);

    VmafPicture pic_2;
    err = vmaf_gpu_picture_pool_fetch(ring_buffer, &pic_2);
    mu_assert("problem during vmaf_picture_pool_request_picture", !err);
    mu_assert("data[0] should have been allocated", pic_2.data[0]);
    mu_assert("data[1] should not have been allocated", !pic_2.data[1]);
    mu_assert("data[2] should not have been allocated", !pic_2.data[2]);
    mu_assert("pix_fmt should be VMAF_PIX_FMT_YUV400P", pic_2.pix_fmt == VMAF_PIX_FMT_YUV400P);

    VmafPicture pic_3;
    err = vmaf_gpu_picture_pool_fetch(ring_buffer, &pic_3);
    mu_assert("problem during vmaf_picture_pool_request_picture", !err);
    mu_assert("data[0] should have been allocated", pic_3.data[0]);
    mu_assert("data[1] should not have been allocated", !pic_3.data[1]);
    mu_assert("data[2] should not have been allocated", !pic_3.data[2]);
    mu_assert("pix_fmt should be VMAF_PIX_FMT_YUV400P", pic_3.pix_fmt == VMAF_PIX_FMT_YUV400P);

    VmafPicture pic_4;
    err = vmaf_gpu_picture_pool_fetch(ring_buffer, &pic_4);
    mu_assert("problem during vmaf_picture_pool_request_picture", !err);
    mu_assert("data[0] should have been allocated", pic_4.data[0]);
    mu_assert("data[1] should not have been allocated", !pic_4.data[1]);
    mu_assert("data[2] should not have been allocated", !pic_4.data[2]);
    mu_assert("pix_fmt should be VMAF_PIX_FMT_YUV400P", pic_4.pix_fmt == VMAF_PIX_FMT_YUV400P);

    VmafPicture pic_5;
    err = vmaf_gpu_picture_pool_fetch(ring_buffer, &pic_5);
    mu_assert("problem during vmaf_picture_pool_request_picture", !err);
    mu_assert("data[0] should have been allocated", pic_5.data[0]);
    mu_assert("data[1] should not have been allocated", !pic_5.data[1]);
    mu_assert("data[2] should not have been allocated", !pic_5.data[2]);
    mu_assert("pix_fmt should be VMAF_PIX_FMT_YUV400P", pic_5.pix_fmt == VMAF_PIX_FMT_YUV400P);

    mu_assert("pic_5 should use the same data buffer as pic_1 did.",
              pic_1.data[0] == pic_5.data[0]);

    err = vmaf_gpu_picture_pool_close(ring_buffer);
    mu_assert("problem during vmaf_gpu_picture_pool_close", !err);

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_ring_buffer);
    return NULL;
}
