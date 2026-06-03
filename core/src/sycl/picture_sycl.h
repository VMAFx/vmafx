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

#ifndef __VMAF_SRC_SYCL_PICTURE_SYCL_H__
#define __VMAF_SRC_SYCL_PICTURE_SYCL_H__

#include "common.h"
#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Cookie attached to VmafPicture instances that own SYCL device memory.
 * Used by the pre-allocation and picture management APIs.
 */
enum VmafSyclPoolMethod {
    VMAF_SYCL_POOL_DEVICE = 0,
    VMAF_SYCL_POOL_HOST = 1,
};

typedef struct VmafSyclCookie {
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w, h;
    VmafSyclState *state;
    enum VmafSyclPoolMethod method;
} VmafSyclCookie;

/**
 * Upload a single Y-plane from a host VmafPicture to a SYCL USM device
 * buffer.  Handles stride != width (packs rows contiguously).
 *
 * @param state  The SYCL state (provides queue for memcpy).
 * @param dst    Device pointer to receive the packed Y-plane data.
 * @param pic    Source host-side VmafPicture.
 * @param plane  Plane index (normally 0 for Y).
 *
 * @return 0 on success, negative errno on failure.
 */
int vmaf_sycl_picture_upload(VmafSyclState *state, void *dst, VmafPicture *pic, unsigned plane);

/**
 * Download a Y-plane from a SYCL USM device buffer back to a host
 * VmafPicture.  Handles stride != width (inserts padding per row).
 *
 * @param state  The SYCL state (provides queue for memcpy).
 * @param src    Device pointer containing packed Y-plane data.
 * @param pic    Destination host-side VmafPicture.
 * @param plane  Plane index (normally 0 for Y).
 *
 * @return 0 on success, negative errno on failure.
 */
int vmaf_sycl_picture_download(VmafSyclState *state, const void *src, VmafPicture *pic,
                               unsigned plane);

/**
 * Allocate a SYCL device-backed VmafPicture.
 * Called by the VmafPicture pre-allocation pool.
 *
 * @param pic     The picture to initialise (data pointers set to USM allocs).
 * @param cookie  Pointer to a VmafSyclCookie with dimensions and state.
 *
 * @return 0 on success, negative errno on failure.
 */
int vmaf_sycl_picture_alloc(VmafPicture *pic, void *cookie);

/**
 * Free a SYCL device-backed VmafPicture.
 * Called by VmafPicture release callback.
 *
 * @param pic     The picture to free.
 * @param cookie  Pointer to a VmafSyclCookie.
 *
 * @return 0 on success, negative errno on failure.
 */
int vmaf_sycl_picture_free(VmafPicture *pic, void *cookie);

/**
 * Pool of pre-allocated USM-backed VmafPictures. Round-robin fetch via
 * refcount sharing — caller receives a reference to one of the N
 * underlying pictures, writes into pic->data[0], passes to
 * vmaf_read_pictures(); SYCL's in-order copy_queue serialises uploads so
 * no per-picture fence is needed on the fetch path.
 *
 * All pictures in the pool are allocated via vmaf_sycl_picture_alloc()
 * (DEVICE) or vmaf_sycl_malloc_host()-wrapped (HOST). Freed via the
 * matching cb on vmaf_sycl_picture_pool_close().
 */
typedef struct VmafSyclPicturePool VmafSyclPicturePool;

/**
 * Create a pool of pic_cnt USM-backed pictures for the given frame
 * dimensions.
 *
 * @param[out] pool     Receives the allocated pool.
 * @param state         SYCL state (provides queue for malloc).
 * @param pic_cnt       Number of pictures in the pool (>= 1).
 * @param w, h          Frame dimensions in pixels.
 * @param bpc           Bits per component (8 or 10).
 * @param pix_fmt       Pixel format (Y-plane only is allocated).
 * @param method        DEVICE (sycl::malloc_device) or HOST (sycl::malloc_host).
 *
 * @return 0 on success, negative errno on failure.
 */
int vmaf_sycl_picture_pool_init(VmafSyclPicturePool **pool, VmafSyclState *state, unsigned pic_cnt,
                                unsigned w, unsigned h, unsigned bpc, enum VmafPixelFormat pix_fmt,
                                enum VmafSyclPoolMethod method);

/**
 * Fetch a reference to the next picture in the pool. Caller owns the
 * returned VmafPicture ref and must release it via vmaf_picture_unref()
 * when done.
 *
 * @param pool  The pool.
 * @param[out] pic  Receives the picture reference.
 *
 * @return 0 on success, negative errno on failure.
 */
int vmaf_sycl_picture_pool_fetch(VmafSyclPicturePool *pool, VmafPicture *pic);

/**
 * Release all resources owned by the pool (USM buffers + metadata).
 * Callers must ensure no outstanding picture refs remain.
 */
int vmaf_sycl_picture_pool_close(VmafSyclPicturePool *pool);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_SRC_SYCL_PICTURE_SYCL_H__ */
