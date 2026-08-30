# Validating `vmaf_sycl_import_d3d11_surface` in a local Windows VM

This doc is a reproducer for the Windows-only surface-import path added in
ADR-0103. CI does not exercise it (the fork's Windows CI is MinGW + no-SYCL,
and the D3D11 API requires Intel oneAPI DPC++ on Windows). Validation
happens manually in a local Windows VM.

## When you need to run this

- You changed [core/src/sycl/d3d11_import.cpp](../../core/src/sycl/d3d11_import.cpp).
- You changed [core/src/sycl/common.cpp](../../core/src/sycl/common.cpp)
  in a way that affects `vmaf_sycl_upload_plane` (the import path's sink).
- You bumped the Intel oneAPI DPC++ toolkit version.

If none of the above, the Linux SYCL + Windows MinGW no-SYCL CI jobs are
enough.

## Prerequisites on the Windows VM

- Windows 10/11 (x64) with an Intel GPU. An iGPU (Xe, UHD Graphics) is
  fine — the D3D11 path doesn't need a discrete Arc.
- [Intel oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit-download.html)
  — installs `icx` / `icpx` and the SYCL runtime.
- Git for Windows, or a network share mounting the repo from the host.
- Meson + Ninja (`pip install meson ninja`).

## Build

```powershell
# From an "Intel oneAPI command prompt" (icpx in PATH + env vars set)
cd C:\path\to\vmaf
meson setup build-sycl libvmaf ^
    -Denable_cuda=false -Denable_sycl=true ^
    --buildtype release
ninja -C build-sycl
```

If the build fails at the link step with an unknown `-fsycl` flag, your
environment is dropping the Intel oneAPI wrappers — re-open the shell via
the oneAPI launcher and retry.

## Smoke test (EINVAL paths)

The EINVAL paths exercise the argument validation without needing a real
decoder. Paste this into `test_d3d11_smoke.c` on the VM:

```c
#include <stdio.h>
#include <libvmaf/libvmaf_sycl.h>

int main(void) {
    /* Every invalid-argument call must return < 0 without crashing. */
    int bad = vmaf_sycl_import_d3d11_surface(NULL, NULL, NULL, 0, 0, 0, 0, 0);
    printf("null pointers => %d\n", bad);

    /* Add your own positive-path test with a real ID3D11Texture2D once
     * you have a D3D11 device set up — see the "Full reproducer" section
     * below. */
    return bad < 0 ? 0 : 1;
}
```

Build + run:

```powershell
icx test_d3d11_smoke.c ^
    -I build-sycl\include /link build-sycl\src\libvmaf.lib
.\test_d3d11_smoke.exe
```

Expected: prints `null pointers => -22` (`-EINVAL`) and exits 0.

## Full reproducer (real D3D11 surface)

For the actual import path you need a real decoded surface. The fastest
route is to use MediaFoundation's H.264 decoder on a short clip — it
outputs `IMFSample`s backed by `ID3D11Texture2D`.

```c
/* pseudocode — full code lives in scripts/test-windows-d3d11-import.c */
ID3D11Device *dev; ID3D11DeviceContext *ctx;
D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                  NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);

/* Decode one frame, extract its ID3D11Texture2D via
 * IMFDXGIBuffer::GetResource. Call it tex. */

VmafSyclState *sycl_state;
VmafSyclConfig sycl_cfg = { .device_index = 0 };
vmaf_sycl_state_init(&sycl_state, &sycl_cfg);
vmaf_sycl_init_frame_buffers(sycl_state, 1920, 1080, 8);

int rc = vmaf_sycl_import_d3d11_surface(sycl_state, dev, tex,
                                        /* subresource */ 0,
                                        /* is_ref */ 1,
                                        1920, 1080, 8);
printf("import rc = %d\n", rc);  /* expect 0 */
```

Record the timing: a 1080p8 frame should take ≲ 2 ms end-to-end on a
typical iGPU + PCIe Gen3 x8 path. An order-of-magnitude slower result
suggests the staging tex isn't in CPU-accessible memory or that the SYCL
H2D path is stalling behind a previous submit.

## Reporting results

Paste into the PR description:

- Windows build: SHA + oneAPI toolkit version.
- Smoke test: pass / fail + the `errno` values seen.
- Full reproducer: frame count processed, mean per-frame μs, which GPU.

That's enough evidence for review — no screen recording, no log upload.
