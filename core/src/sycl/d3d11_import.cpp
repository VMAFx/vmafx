/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Windows D3D11 surface import for the SYCL backend.
 *
 *  Decoded video surfaces on Windows are typically ID3D11Texture2D handles
 *  in GPU memory. This TU implements the host staging round-trip that
 *  ADR-0103 chose over a zero-copy shared-NT-handle path:
 *
 *    source texture (GPU, D3D11_USAGE_DEFAULT)
 *        └── CopyResource ──▶ staging tex (D3D11_USAGE_STAGING, CPU_READ)
 *                                   └── Map(D3D11_MAP_READ) ──▶ mapped.pData
 *                                                                    │
 *                                                       memcpy (CPU row pitch)
 *                                                                    ▼
 *                                                      SYCL shared buffer (USM)
 *                                                      via vmaf_sycl_upload_plane
 *
 *  This is NOT zero-copy. Throughput is bounded by:
 *    - GPU→CPU staging Map (≈ PCIe upstream)
 *    - CPU→GPU SYCL H2D memcpy (≈ PCIe downstream)
 *
 *  Zero-copy would need DXGI NT-handle sharing + cross-API interop. Intel
 *  oneAPI DPC++ doesn't yet document ID3D11Resource import in SYCL; revisit
 *  when that lands.
 *
 *  This TU is .cpp (icpx-cl drives it as C++ on Windows) and uses
 *  C++ method-call syntax for COM interfaces (`device->CreateTexture2D(...)`)
 *  — d3d11.h's COBJMACROS C-style helpers are gated behind
 *  `#if !defined(__cplusplus)`, so they aren't visible here. The two
 *  forms are ABI-equivalent (both dispatch through the COM vtable);
 *  the choice is purely lexical. See ADR-0103 rationale.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <libvmaf/libvmaf_sycl.h>

/* log.h is internal to libvmaf/src/, not part of the public
 * libvmaf/include/libvmaf/ surface — bare include via the
 * src-relative path supplied as -I in the icpx invocation.
 * Wrapped in extern "C" because log.h has no __cplusplus guard
 * (upstream Netflix header) and vmaf_log must resolve to the
 * C-linkage symbol produced by log.c. */
#include "log.h"

/* libvmaf_sycl.h declares these with C linkage already. Just forward them
 * so this TU doesn't need the internal common.h. */
extern "C" int vmaf_sycl_upload_plane(VmafSyclState *state, const void *src, unsigned pitch,
                                      int is_ref, unsigned w, unsigned h, unsigned bpc);

extern "C" int vmaf_sycl_import_d3d11_surface(VmafSyclState *state, void *d3d11_device_ptr,
                                              void *d3d11_texture_ptr, unsigned subresource,
                                              int is_ref, unsigned w, unsigned h, unsigned bpc)
{
    if (!state || !d3d11_device_ptr || !d3d11_texture_ptr)
        return -EINVAL;
    if (w == 0 || h == 0)
        return -EINVAL;
    if (bpc != 8 && bpc != 10)
        return -EINVAL;

    ID3D11Device *device = (ID3D11Device *)d3d11_device_ptr;
    ID3D11Texture2D *src_tex = (ID3D11Texture2D *)d3d11_texture_ptr;

    D3D11_TEXTURE2D_DESC src_desc;
    memset(&src_desc, 0, sizeof(src_desc));
    src_tex->GetDesc(&src_desc);

    if (src_desc.Width < w || src_desc.Height < h) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "D3D11 import: source texture %ux%u smaller than requested %ux%u\n",
                 (unsigned)src_desc.Width, (unsigned)src_desc.Height, w, h);
        return -EINVAL;
    }

    /* Fast path: the caller already handed us a staging texture that the
     * CPU can Map directly. Skip the CopyResource round-trip. */
    const bool src_is_staging = src_desc.Usage == D3D11_USAGE_STAGING &&
                                (src_desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0;

    ID3D11Texture2D *staging_tex = NULL;
    ID3D11DeviceContext *ctx = NULL;
    int rc = 0;

    device->GetImmediateContext(&ctx);
    if (!ctx) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: GetImmediateContext returned NULL\n");
        return -EIO;
    }

    ID3D11Resource *map_target;
    if (src_is_staging) {
        map_target = (ID3D11Resource *)src_tex;
    } else {
        D3D11_TEXTURE2D_DESC staging_desc = src_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        staging_desc.ArraySize = 1;
        staging_desc.MipLevels = 1;

        HRESULT hr = device->CreateTexture2D(&staging_desc, NULL, &staging_tex);
        if (FAILED(hr)) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "D3D11 import: CreateTexture2D(staging) failed: 0x%08lx\n", (unsigned long)hr);
            rc = -EIO;
            goto done;
        }
        ctx->CopyResource((ID3D11Resource *)staging_tex, (ID3D11Resource *)src_tex);
        map_target = (ID3D11Resource *)staging_tex;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    memset(&mapped, 0, sizeof(mapped));
    /* When we allocated our own staging texture it has ArraySize=1 and
     * MipLevels=1 so subresource 0 is always the right slice. When the
     * caller passed their own staging texture honour their subresource
     * index. */
    const unsigned map_sub = src_is_staging ? subresource : 0;
    HRESULT hr = ctx->Map(map_target, map_sub, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: Map(staging) failed: 0x%08lx\n",
                 (unsigned long)hr);
        rc = -EIO;
        goto done;
    }

    if (!mapped.pData || mapped.RowPitch == 0) {
        ctx->Unmap(map_target, map_sub);
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: Map returned empty descriptor\n");
        rc = -EIO;
        goto done;
    }

    rc = vmaf_sycl_upload_plane(state, mapped.pData, mapped.RowPitch, is_ref, w, h, bpc);
    ctx->Unmap(map_target, map_sub);

done:
    if (staging_tex)
        staging_tex->Release();
    if (ctx)
        ctx->Release();
    return rc;
}

#endif /* _WIN32 */
