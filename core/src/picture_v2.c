/**
 * Copyright 2026 Lusoris
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
 */

/**
 * @file picture_v2.c
 *
 * VmafPicture v2 implementation (cycle N+1, ADR-0928).
 *
 * All four allocation / conversion entry points are implemented here.
 * `vmaf_backend_handle_name` is a static lookup table — no allocations.
 * `vmaf_picture2_alloc` / `vmaf_picture2_unref` are thin wrappers around
 * the existing v1 helpers in picture.c. The two converters copy the
 * shared fields and increment the underlying reference count so both
 * the source and the destination picture must be independently unref'd.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/picture.h"
#include "libvmaf/picture_v2.h"
#include "picture.h"
#include "ref.h"

/* -------------------------------------------------------------------------
 * vmaf_backend_handle_name
 * ---------------------------------------------------------------------- */

const char *vmaf_backend_handle_name(VmafBackendHandle backend)
{
    /* Stable static literals — the returned pointer must never be freed. */
    static const char *const names[VMAF_BACKEND_HANDLE__COUNT] = {
        [VMAF_BACKEND_HANDLE_NONE] = "none",   [VMAF_BACKEND_HANDLE_CUDA] = "cuda",
        [VMAF_BACKEND_HANDLE_SYCL] = "sycl",   [VMAF_BACKEND_HANDLE_HIP] = "hip",
        [VMAF_BACKEND_HANDLE_METAL] = "metal", [VMAF_BACKEND_HANDLE_VULKAN] = "vulkan",
    };

    if ((unsigned)backend >= VMAF_BACKEND_HANDLE__COUNT)
        return "unknown";
    return names[(unsigned)backend];
}

/* -------------------------------------------------------------------------
 * vmaf_picture2_alloc
 *
 * Allocate a CPU-backed v2 picture. Delegates to vmaf_picture_alloc()
 * for the v1 plane allocations, then overlays the v2-specific fields.
 * ---------------------------------------------------------------------- */

int vmaf_picture2_alloc(VmafPicture2 *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                        unsigned h)
{
    if (!pic)
        return -EINVAL;

    /* Allocate the v1 backing.  The v1 fields (pix_fmt, bpc, w, h, stride,
     * data, ref, priv) share the same layout prefix as VmafPicture2. */
    VmafPicture v1;
    memset(&v1, 0, sizeof(v1));
    int err = vmaf_picture_alloc(&v1, pix_fmt, bpc, w, h);
    if (err)
        return err;

    /* Copy all v1 fields into the v2 struct. */
    memset(pic, 0, sizeof(*pic));
    pic->pix_fmt = v1.pix_fmt;
    pic->bpc = v1.bpc;
    for (unsigned i = 0; i < 3; i++) {
        pic->w[i] = v1.w[i];
        pic->h[i] = v1.h[i];
        pic->stride[i] = v1.stride[i];
        pic->data[i] = v1.data[i];
    }
    pic->ref = v1.ref;
    pic->priv = v1.priv;

    /* v2-specific: CPU picture has no device handle. */
    pic->backend = VMAF_BACKEND_HANDLE_NONE;
    pic->backend_handle = 0;
    /* _reserved[4] already zeroed by memset above. */

    return 0;
}

/* -------------------------------------------------------------------------
 * vmaf_picture2_unref
 *
 * Decrement the underlying refcount and free plane buffers when it
 * reaches zero. Converts to a transient VmafPicture v1 view and
 * delegates to vmaf_picture_unref().
 * ---------------------------------------------------------------------- */

int vmaf_picture2_unref(VmafPicture2 *pic)
{
    if (!pic)
        return 0; /* NULL is a documented no-op. */
    if (!pic->ref)
        return -EINVAL;

    /* Construct a transient v1 view for the unref call.  The v1 view
     * shares the same plane-pointer and ref values as the v2 struct; the
     * release callback stored in priv holds the actual deallocation logic
     * and is backend-agnostic. */
    VmafPicture v1;
    memset(&v1, 0, sizeof(v1));
    v1.pix_fmt = pic->pix_fmt;
    v1.bpc = pic->bpc;
    for (unsigned i = 0; i < 3; i++) {
        v1.w[i] = pic->w[i];
        v1.h[i] = pic->h[i];
        v1.stride[i] = pic->stride[i];
        v1.data[i] = pic->data[i];
    }
    v1.ref = pic->ref;
    v1.priv = pic->priv;

    int err = vmaf_picture_unref(&v1);
    if (!err)
        memset(pic, 0, sizeof(*pic));
    return err;
}

/* -------------------------------------------------------------------------
 * vmaf_picture_v1_to_v2
 *
 * Promote a VmafPicture v1 to a VmafPicture2.  The v1 source's refcount
 * is incremented; the caller still owns src and must vmaf_picture_unref()
 * it independently. The destination receives backend = NONE / handle = 0.
 * ---------------------------------------------------------------------- */

int vmaf_picture_v1_to_v2(const VmafPicture *src, VmafPicture2 *dst)
{
    if (!src || !dst)
        return -EINVAL;
    if (!src->ref)
        return -EINVAL;

    memset(dst, 0, sizeof(*dst));
    dst->pix_fmt = src->pix_fmt;
    dst->bpc = src->bpc;
    for (unsigned i = 0; i < 3; i++) {
        dst->w[i] = src->w[i];
        dst->h[i] = src->h[i];
        dst->stride[i] = src->stride[i];
        dst->data[i] = src->data[i];
    }
    dst->ref = src->ref;
    dst->priv = src->priv;

    /* Increment refcount so the v2 picture is a true independent reference. */
    vmaf_ref_fetch_increment(src->ref);

    dst->backend = VMAF_BACKEND_HANDLE_NONE;
    dst->backend_handle = 0;

    return 0;
}

/* -------------------------------------------------------------------------
 * vmaf_picture_v2_to_v1
 *
 * Demote a VmafPicture2 to a VmafPicture v1.  The backend discriminator
 * and handle are dropped — inspect src before conversion if you need them.
 * The v2 source's refcount is incremented; the caller still owns src and
 * must vmaf_picture2_unref() it independently.
 * ---------------------------------------------------------------------- */

int vmaf_picture_v2_to_v1(const VmafPicture2 *src, VmafPicture *dst)
{
    if (!src || !dst)
        return -EINVAL;
    if (!src->ref)
        return -EINVAL;

    memset(dst, 0, sizeof(*dst));
    dst->pix_fmt = src->pix_fmt;
    dst->bpc = src->bpc;
    for (unsigned i = 0; i < 3; i++) {
        dst->w[i] = src->w[i];
        dst->h[i] = src->h[i];
        dst->stride[i] = src->stride[i];
        dst->data[i] = src->data[i];
    }
    dst->ref = src->ref;
    dst->priv = src->priv;

    /* Increment refcount so the v1 picture is a true independent reference. */
    vmaf_ref_fetch_increment(src->ref);

    return 0;
}
