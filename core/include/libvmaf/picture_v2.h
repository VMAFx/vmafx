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

/**
 * @file picture_v2.h
 *
 * VmafPicture v2 — explicit per-backend GPU-state carrier.
 *
 * @warning DESIGN HEADER. The symbols declared here are NOT yet linked
 *          into libvmaf.so. This header ships in the source tree to
 *          establish the v2 surface for downstream consumers (FFmpeg
 *          patches, Rust binding, MCP server, controller/node protocol)
 *          to review and plan against. Implementation lands in a
 *          follow-up PR (see ADR-0928 §Consequences, "Cycle N+1").
 *
 * Differences from v1 (`picture.h`):
 *   - Explicit `backend` enum discriminator — answers "which backend
 *     produced this picture?" without casting `void *priv`.
 *   - Typed `backend_handle` `uintptr_t` slot — replaces the
 *     `void *priv` overlay pattern that CUDA, SYCL, HIP, and Metal
 *     each independently re-invented.
 *   - Reserved tail (`_reserved[4]`) preserves ABI additive growth
 *     room for the v2 lifecycle.
 *
 * Migration:
 *   - v1 (`VmafPicture`) is preserved unchanged for the 12-month
 *     deprecation window (see ADR-0928).
 *   - Convert v1 ↔ v2 via `vmaf_picture_v1_to_v2` / `vmaf_picture_v2_to_v1`
 *     (implementations land in cycle N+1).
 *   - The SONAME bump from libvmaf.so.3 → libvmaf.so.4 is scheduled
 *     for VMAFX v4.0.0 (cycle N+3), not this PR.
 *
 * See:
 *   - [ADR-0928](../../docs/adr/0928-vmaf-picture-v2-explicit-backend-state.md)
 *   - [docs/architecture/vmaf-picture-v2-migration.md](../../docs/architecture/vmaf-picture-v2-migration.md)
 */

#ifndef LIBVMAF_PICTURE_V2_H_
#define LIBVMAF_PICTURE_V2_H_

#include <stddef.h>
#include <stdint.h>

#include <libvmaf/macros.h>
#include <libvmaf/picture.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Explicit backend discriminator carried on every v2 picture.
 *
 * The value identifies which backend allocated and currently owns the
 * device-side resources referenced by `backend_handle`. Used by
 * cross-backend dispatch to fail-fast on misroute (e.g., a CUDA picture
 * fed to a SYCL extractor returns `-EINVAL` instead of `void *` UB).
 *
 * Stable numeric values — additions go at the end before
 * `VMAF_BACKEND_HANDLE__COUNT`. Never renumber existing entries.
 */
typedef enum VmafBackendHandle {
    VMAF_BACKEND_HANDLE_NONE = 0,   /**< CPU-resident, no device handle. */
    VMAF_BACKEND_HANDLE_CUDA = 1,   /**< `backend_handle` is `CUstream`. */
    VMAF_BACKEND_HANDLE_SYCL = 2,   /**< `backend_handle` is a `VmafSyclState *` cookie. */
    VMAF_BACKEND_HANDLE_HIP = 3,    /**< `backend_handle` is `hipStream_t`. */
    VMAF_BACKEND_HANDLE_METAL = 4,  /**< `backend_handle` is `id<MTLCommandQueue>` (bridged). */
    VMAF_BACKEND_HANDLE_VULKAN = 5, /**< Reserved — Vulkan import (ADR-0186 / ADR-0726). */
    VMAF_BACKEND_HANDLE__COUNT      /**< Sentinel — not a valid handle. */
} VmafBackendHandle;

/**
 * VmafPicture2 — picture descriptor with explicit GPU-backend state.
 *
 * Layout discipline (matches `VmafPicture` v1 prefix where possible to
 * keep mental-model and converter cost low):
 *   - `pix_fmt`, `bpc`, `w`, `h`, `stride`, `data`, `ref` mirror v1.
 *   - `backend` + `backend_handle` are new and explicit.
 *   - `priv` retained for core-internal bookkeeping (the converter
 *     allocates it during v1 → v2 promotion).
 *   - `_reserved[4]` is zero-initialised; future additive growth
 *     consumes from this tail without changing struct size.
 *
 * Ownership:
 *   - `data[]` plane pointers are owned by the producing backend's
 *     allocator and freed via `vmaf_picture2_unref`.
 *   - `backend_handle` is a *non-owning* reference — the backend
 *     itself owns the stream/queue lifecycle.
 *   - `ref` mirrors v1 semantics (atomic refcount via `VmafRef`).
 *   - `priv` is core-owned; consumers must never `free()` it.
 */
typedef struct VmafPicture2 {
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w[3], h[3];
    ptrdiff_t stride[3];
    void *data[3];
    VmafRef *ref;
    void *priv;
    VmafBackendHandle backend;
    uintptr_t backend_handle;
    uintptr_t _reserved[4];
} VmafPicture2;

/**
 * Type alias for forward-compatibility. Once v1 is removed
 * (cycle N+3 / SONAME 4), `VmafPicture` becomes a typedef for
 * `VmafPicture2`. Until then, the typedef is intentionally absent —
 * callers opt in explicitly by typing `VmafPicture2`.
 */

/**
 * Allocate a CPU-backed v2 picture. Equivalent to
 * `vmaf_picture_alloc` v1 but populates
 * `backend = VMAF_BACKEND_HANDLE_NONE` and
 * `backend_handle = 0`.
 *
 * Not yet implemented — see ADR-0928 §Consequences (cycle N+1).
 *
 * @param pic     Out: receives the populated picture descriptor. Must not be NULL.
 * @param pix_fmt Planar pixel format. `VMAF_PIX_FMT_UNKNOWN` is rejected.
 * @param bpc     Bits per component (8, 10, 12, or 16).
 * @param w       Luma width in samples. Must be > 0.
 * @param h       Luma height in samples. Must be > 0.
 *
 * @return 0 on success, negative errno on failure.
 *         `-ENOSYS` until the cycle-N+1 implementation PR lands.
 *         `-EINVAL` for NULL pointer or unknown format.
 *         `-ENOMEM` on allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext (and its pictures) per thread.
 */
VMAF_EXPORT int vmaf_picture2_alloc(VmafPicture2 *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                    unsigned w, unsigned h);

/**
 * Release a v2 picture. Decrements the underlying refcount and frees
 * plane buffers when it reaches zero. Does not touch `backend_handle`
 * (the backend owns its stream/queue lifecycle).
 *
 * Not yet implemented — see ADR-0928 §Consequences (cycle N+1).
 *
 * @param pic Picture descriptor to unref. NULL is a no-op.
 *
 * @return 0 on success, negative errno on failure.
 *         `-ENOSYS` until the cycle-N+1 implementation PR lands.
 *
 * @thread-safety Not thread-safe. Use one VmafContext (and its pictures) per thread.
 */
VMAF_EXPORT int vmaf_picture2_unref(VmafPicture2 *pic);

/**
 * Promote a v1 `VmafPicture` to a v2 `VmafPicture2`. The result
 * carries `backend = VMAF_BACKEND_HANDLE_NONE` and
 * `backend_handle = 0` by default; backends that produced the v1
 * picture via their own pool callbacks override these fields during
 * the conversion (see cycle-N+1 implementation notes).
 *
 * The v1 source picture's `ref` is incremented; the caller still
 * owns `src` and must `vmaf_picture_unref` it independently.
 *
 * Not yet implemented — see ADR-0928 §Consequences (cycle N+1).
 *
 * @param src Source v1 picture. Must not be NULL.
 * @param dst Out: receives the populated v2 descriptor. Must not be NULL.
 *
 * @return 0 on success, negative errno on failure.
 *         `-ENOSYS` until the cycle-N+1 implementation PR lands.
 *         `-EINVAL` on NULL arguments.
 *
 * @thread-safety Not thread-safe. Use one VmafContext (and its pictures) per thread.
 */
VMAF_EXPORT int vmaf_picture_v1_to_v2(const VmafPicture *src, VmafPicture2 *dst);

/**
 * Demote a v2 `VmafPicture2` to a v1 `VmafPicture`. The `backend`
 * discriminator and `backend_handle` are dropped — callers that
 * need them must inspect `src` before conversion. Used during the
 * dual-API window to feed v2-produced pictures into v1-only
 * extractors that have not yet migrated.
 *
 * The v2 source picture's `ref` is incremented; the caller still
 * owns `src` and must `vmaf_picture2_unref` it independently.
 *
 * Not yet implemented — see ADR-0928 §Consequences (cycle N+1).
 *
 * @param src Source v2 picture. Must not be NULL.
 * @param dst Out: receives the populated v1 descriptor. Must not be NULL.
 *
 * @return 0 on success, negative errno on failure.
 *         `-ENOSYS` until the cycle-N+1 implementation PR lands.
 *         `-EINVAL` on NULL arguments.
 *
 * @thread-safety Not thread-safe. Use one VmafContext (and its pictures) per thread.
 */
VMAF_EXPORT int vmaf_picture_v2_to_v1(const VmafPicture2 *src, VmafPicture *dst);

/**
 * Return a stable string name for a `VmafBackendHandle` value
 * (e.g., `"cuda"`, `"sycl"`, `"none"`). Useful for error messages
 * and tracing. The returned pointer is to a static literal and
 * must not be freed.
 *
 * Returns `"unknown"` for values outside the enum range.
 *
 * Not yet implemented — see ADR-0928 §Consequences (cycle N+1).
 *
 * @param backend Backend discriminator value to name.
 *
 * @return NUL-terminated static string. Never NULL. Do not free.
 *
 * @thread-safety Safe to call from any thread; reads only static data.
 */
VMAF_EXPORT const char *vmaf_backend_handle_name(VmafBackendHandle backend);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PICTURE_V2_H_ */
