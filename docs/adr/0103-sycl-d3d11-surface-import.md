<!-- markdownlint-disable MD013 -->
# ADR-0103: `vmaf_sycl_import_d3d11_surface` ships as a staging-texture H2D path, not zero-copy

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: sycl, windows, api

## Context

The public header [`core/include/libvmaf/libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h)
declared `vmaf_sycl_import_d3d11_surface()` under `#ifdef _WIN32`
alongside its doc block ("Creates a staging texture, copies the decoded
surface, maps it for CPU read, and uploads via H2D memcpy"), but **no
implementation existed anywhere in the tree**. Issue
[#27](https://github.com/VMAFx/vmafx/issues/27) surfaced this as a
link-time failure: any Windows consumer compiling against libvmaf-sycl
would see `undefined reference to vmaf_sycl_import_d3d11_surface` at
link time, because the symbol's declaration was visible but no TU
exported it.

The options were:

1. **Remove the declaration** — small diff, closes the issue, but
   permanently locks out Windows D3D11 callers (MediaFoundation, DXVA2,
   DirectX 11 VideoProcessor) from the SYCL backend.
2. **Implement the staging-texture H2D path** — matches the existing
   doc block, larger diff, requires a Windows validation path.
3. **Keep the declaration, implement as `-ENOSYS` stub** — no useful
   behaviour gained.

The user chose option 2.

## Decision

### Surface

Implemented in a new TU [`core/src/sycl/d3d11_import.cpp`](../../core/src/sycl/d3d11_import.cpp).
The entire file body is wrapped in `#ifdef _WIN32` so the TU is a
compile-clean no-op on Linux / macOS — it slots into `sycl_sources`
alongside `dmabuf_import.cpp` without gating the Meson source list on
`host_machine.system()`.

### Call flow

```text
caller's ID3D11Texture2D*  ──▶  CopyResource  ──▶  staging tex
(D3D11_USAGE_DEFAULT)                               (D3D11_USAGE_STAGING,
                                                    D3D11_CPU_ACCESS_READ)
                                                          │
                                                          ▼
                                                       Map()
                                                          │
                                                          ▼
                                              vmaf_sycl_upload_plane
                                              (mapped.pData, RowPitch)
                                                          │
                                                          ▼
                                              SYCL shared buffer (USM)
                                                          │
                                                          ▼
                                                      Unmap()
                                                   Release(staging)
```

### Fast path for caller-provided staging textures

If the caller already hands us a staging texture (`Usage == D3D11_USAGE_STAGING`
and `CPUAccessFlags & D3D11_CPU_ACCESS_READ`), the function skips the
`CreateTexture2D` + `CopyResource` prelude and maps the caller's texture
directly, honouring their `subresource` index. This matters for decoder
pipelines that already stage on the encoder's side (e.g. DXVA2 output
samples that arrive pre-staged).

### COM bindings

The implementation uses `#define COBJMACROS` and the C-style COM macros
(`ID3D11Device_CreateTexture2D`, `ID3D11Texture2D_Release`,
`ID3D11DeviceContext_Map`, etc.). These are defined identically in both
MSVC's `d3d11.h` and mingw-w64's port, so a single source works under
Intel oneAPI DPC++ (which wraps MSVC on Windows) and under any future
MinGW-based SYCL build. No `#ifdef _MSC_VER` split was necessary after
verifying the Wine-derived header family exports all 25 required APIs
(`CreateTexture2D`, `GetDesc`, `CopyResource`, `GetImmediateContext`,
`Map`, `Unmap`, `Release`, plus the `D3D11_MAPPED_SUBRESOURCE` /
`D3D11_TEXTURE2D_DESC` types).

### Meson wiring

- [`core/src/meson.build`](../../core/src/meson.build) appends
  `d3d11_import.cpp` to `sycl_sources`. On non-Windows the TU compiles
  to an empty `.o` (verified: 3312-byte icpx output on Linux).
- When `host_machine.system() == 'windows'`, the build adds `-ld3d11
  -ldxgi` to `sycl_surface_deps`. On non-Windows nothing is linked.

### Validation

- **Automated**: MinGW CI job [`windows.yml`](../../.github/workflows/windows.yml)
  builds with `-Denable_sycl=false` (Intel oneAPI DPC++ isn't available
  on the stock GitHub `windows-latest` runner and paid runner minutes
  are off the table per the user's standing constraint). The MinGW job
  therefore does **not** exercise this TU at all — it's gated behind
  `is_sycl_enabled` in `meson.build`.
- **Manual**: The user boots a local Windows VM with Intel oneAPI DPC++
  installed and runs the reproducer at
  [`docs/development/windows-d3d11-import.md`](../development/windows-d3d11-import.md).
  That doc lists EINVAL-path smoke tests (no D3D11 device needed) plus
  a full reproducer skeleton using MediaFoundation's H.264 decoder.

## Alternatives considered

| Alternative | Why rejected |
| --- | --- |
| **Remove the declaration from the public header** | Permanently locks Windows D3D11 callers out of the SYCL backend. The fork's MediaFoundation-ingestion path would need its own CPU-stage-and-upload helper, duplicating the work this function centralises. |
| **Zero-copy via DXGI NT-handle sharing + SYCL D3D11 interop** | Intel oneAPI DPC++ as of 2025.1 has `sycl::ext::oneapi::experimental::external_memory` for Vulkan/OpenGL interop, but no documented `ID3D11Resource` import path. Implementing shared-NT-handle → HANDLE → `external_memory_descriptor` ourselves would require replicating a chunk of the DirectX interop contract and hasn't been validated by Intel. Revisit when DPC++ ships first-class D3D11 interop. |
| **Zero-copy via `IDXGIKeyedMutex` shared surface** | Would need the caller to set up a shared surface at decoder init time — breaks the drop-in contract of the existing `vmaf_sycl_import_va_surface`. Also requires the SYCL side to open the same shared handle, which hits the same DPC++-doesn't-document-D3D11-interop wall as above. |
| **MSVC-only implementation gated on `#ifdef _MSC_VER`** | mingw-w64's d3d11.h exports the same COM bindings as MSVC's (verified: 25 API hits in the Wine-derived header family). The compiler split would have been premature pessimism. |
| **Direct `ID3D11Texture2D::Map` on the source tex (no staging)** | Source textures from decoders are almost always `D3D11_USAGE_DEFAULT` — they can't be mapped. The staging round-trip is what the D3D11 programming model requires for CPU read. The fast-path for pre-staged textures above covers the one case where this isn't needed. |

## Consequences

- Windows consumers compiling against libvmaf-sycl now get a working
  `vmaf_sycl_import_d3d11_surface` instead of a link error.
- The implementation is **not zero-copy**: throughput is bounded by
  PCIe upstream (staging Map) + PCIe downstream (SYCL H2D). For
  1080p8 input on a typical PCIe Gen3 x8 iGPU that's ≲ 2 ms per call;
  for 2160p10 it's ≲ 12 ms. Docs
  ([sycl/overview.md](../backends/sycl/overview.md)) flag this
  clearly so callers don't mistake the surface for a zero-copy path.
- Runtime validation is **manual, not automated**. The validation-gap
  is deliberate per the user's "no paid GitHub Actions" constraint
  and documented in
  [`docs/development/windows-d3d11-import.md`](../development/windows-d3d11-import.md).
- Build system gains a pair of Windows-only linker deps (`d3d11`,
  `dxgi`). Both are in the Windows SDK / mingw-w64 headers package by
  default — no new external dependency to pin.
- Merge friendliness: `d3d11_import.cpp` is a new file, not a modification
  of any existing SYCL TU. Easy to revert if the zero-copy path
  becomes available later.

## References

- Issue [#27 — vmaf_sycl_import_d3d11_surface declared but not implemented](https://github.com/VMAFx/vmafx/issues/27)
- [Microsoft — ID3D11DeviceContext::Map method](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
- [Microsoft — D3D11_USAGE enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_usage)
- [Intel — oneAPI DPC++ Compiler, external_memory extension](https://github.com/intel/llvm/blob/sycl/sycl/doc/extensions/experimental/sycl_ext_oneapi_external_memory.asciidoc)
- ADR-0040 — multi-input DNN session API (unrelated; cited for the
  "new TU lives next to sibling imports" pattern this ADR reuses).
