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

#ifndef __VMAF_SRC_PICTURE_POOL_H__
#define __VMAF_SRC_PICTURE_POOL_H__

#include "picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for a CPU-side picture pool.
 *
 * @var VmafPicturePoolConfig::pic_cnt  Number of pre-allocated picture slots.
 * @var VmafPicturePoolConfig::w        Picture width in pixels.
 * @var VmafPicturePoolConfig::h        Picture height in pixels.
 * @var VmafPicturePoolConfig::pix_fmt  Pixel format for all slots.
 * @var VmafPicturePoolConfig::bpc      Bits per component (8 or 10).
 */
typedef struct VmafPicturePoolConfig {
    unsigned pic_cnt;
    unsigned w;
    unsigned h;
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
} VmafPicturePoolConfig;

/** @brief Opaque CPU picture pool handle. */
typedef struct VmafPicturePool VmafPicturePool;

/**
 * @brief Allocate and initialise a picture pool.
 *
 * @param[out] pool  Receives the pool on success.
 * @param cfg        Pool configuration (copied by value).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_picture_pool_init(VmafPicturePool **pool, VmafPicturePoolConfig cfg);

/**
 * @brief Destroy a picture pool and free all picture memory.
 *
 * @param pool  Pool to close.  May be NULL (no-op).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_picture_pool_close(VmafPicturePool *pool);

/**
 * @brief Fetch the next available picture slot, blocking until one is free.
 *
 * The returned picture is reference-counted; the caller must call
 * vmaf_picture_unref() when done to return it to the pool.
 *
 * @param pool      Picture pool.
 * @param[out] pic  Populated with the fetched picture on success.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_picture_pool_fetch(VmafPicturePool *pool, VmafPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_SRC_PICTURE_POOL_H__ */
