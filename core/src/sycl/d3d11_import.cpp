/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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

static int copy_to_staging(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *src_tex,
                           const D3D11_TEXTURE2D_DESC &src_desc, ID3D11Texture2D **out_staging)
{
    D3D11_TEXTURE2D_DESC staging_desc = src_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.ArraySize = 1;
    staging_desc.MipLevels = 1;

    HRESULT hr = device->CreateTexture2D(&staging_desc, NULL, out_staging);
    if (FAILED(hr)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: CreateTexture2D(staging) failed: 0x%08lx\n",
                 (unsigned long)hr);
        return -EIO;
    }
    ctx->CopyResource((ID3D11Resource *)*out_staging, (ID3D11Resource *)src_tex);
    return 0;
}

static int map_and_upload(VmafSyclState *state, ID3D11DeviceContext *ctx,
                          ID3D11Resource *map_target, unsigned map_sub, int is_ref, unsigned w,
                          unsigned h, unsigned bpc)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    memset(&mapped, 0, sizeof(mapped));
    HRESULT hr = ctx->Map(map_target, map_sub, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: Map(staging) failed: 0x%08lx\n",
                 (unsigned long)hr);
        return -EIO;
    }

    int rc = 0;
    if (!mapped.pData || mapped.RowPitch == 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: Map returned empty descriptor\n");
        rc = -EIO;
    } else {
        rc = vmaf_sycl_upload_plane(state, mapped.pData, mapped.RowPitch, is_ref, w, h, bpc);
    }

    ctx->Unmap(map_target, map_sub);
    return rc;
}

extern "C" int vmaf_sycl_import_d3d11_surface(VmafSyclState *state, void *d3d11_device_ptr,
                                              void *d3d11_texture_ptr, unsigned subresource,
                                              int is_ref, unsigned w, unsigned h, unsigned bpc)
{
    if (!state || !d3d11_device_ptr || !d3d11_texture_ptr)
        return -EINVAL;
    if (w == 0 || h == 0 || (bpc != 8 && bpc != 10))
        return -EINVAL;

    auto *device = static_cast<ID3D11Device *>(d3d11_device_ptr);
    auto *src_tex = static_cast<ID3D11Texture2D *>(d3d11_texture_ptr);

    D3D11_TEXTURE2D_DESC src_desc;
    memset(&src_desc, 0, sizeof(src_desc));
    src_tex->GetDesc(&src_desc);

    if (src_desc.Width < w || src_desc.Height < h) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "D3D11 import: source texture %ux%u smaller than requested %ux%u\n",
                 (unsigned)src_desc.Width, (unsigned)src_desc.Height, w, h);
        return -EINVAL;
    }

    ID3D11DeviceContext *ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "D3D11 import: GetImmediateContext returned NULL\n");
        return -EIO;
    }

    const bool src_is_staging = src_desc.Usage == D3D11_USAGE_STAGING &&
                                (src_desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0;

    ID3D11Texture2D *staging_tex = nullptr;
    ID3D11Resource *map_target = nullptr;
    unsigned map_sub = 0;
    int rc = 0;

    if (src_is_staging) {
        map_target = (ID3D11Resource *)src_tex;
        map_sub = subresource;
    } else {
        rc = copy_to_staging(device, ctx, src_tex, src_desc, &staging_tex);
        if (rc == 0) {
            map_target = (ID3D11Resource *)staging_tex;
            map_sub = 0;
        }
    }

    if (rc == 0)
        rc = map_and_upload(state, ctx, map_target, map_sub, is_ref, w, h, bpc);

    if (staging_tex)
        staging_tex->Release();
    ctx->Release();
    return rc;
}

#else /* !_WIN32 */

#include <cerrno>

#include <libvmaf/libvmaf_sycl.h>

/* libvmaf_sycl.h declares the import on every platform and the library's
 * version script lists it (vmafx_legacy_sycl.map, ADR-2094): off Windows it
 * is defined and refuses, as a D3D11 texture cannot exist there. */
extern "C" int vmaf_sycl_import_d3d11_surface(VmafSyclState *state, void *d3d11_device_ptr,
                                              void *d3d11_texture_ptr, unsigned subresource,
                                              int is_ref, unsigned w, unsigned h, unsigned bpc)
{
    (void)state;
    (void)d3d11_device_ptr;
    (void)d3d11_texture_ptr;
    (void)subresource;
    (void)is_ref;
    (void)w;
    (void)h;
    (void)bpc;
    return -ENOSYS;
}

#endif /* _WIN32 */
