<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0761 — Metal Backend Audit (2026-05-29)

## Purpose

Full audit of `core/src/metal/` (Obj-C++ runtime, picture, IOSurface import,
kernel template, dispatch strategy) and `core/src/feature/metal/` (`.mm` host
binders + `.metal` MSL shaders) across eight concern classes:

1. Apple-Family-7 gate correctness.
2. ARC correctness (no manual retain/release in ARC-compiled TUs).
3. MTLFunction lookup safety (nil-check after `newFunctionWithName:`).
4. IOSurface lifecycle (analogous to the pinned-host leak in HIP/PR #94).
5. MSL struct-by-value in kernel signatures.
6. Scaffold vs real-kernel classification.
7. Dispatch table completeness when a new kernel lands.
8. Public-header Doxygen status block currency.

## Scope

Files read:

- `core/src/metal/{common,picture_metal,picture_import,kernel_template,dispatch_strategy}.{mm,c,h}`
- `core/src/metal/{state_priv,import,kernel_template,common,picture_metal,dispatch_strategy}.h`
- `core/src/metal/meson.build`
- `core/src/feature/metal/*.{mm,metal}` (9 `.mm` host TUs, 8 `.metal` shader files)
- `core/src/metal/AGENTS.md`, `core/src/feature/metal/AGENTS.md`
- `core/include/libvmaf/libvmaf_metal.h`
- `core/src/feature/feature_extractor.c` (HAVE_METAL block)

---

## Inventory

### Runtime (`core/src/metal/`)

| File | Status |
|------|--------|
| `common.mm` | Real (T8-1b / ADR-0420) |
| `picture_metal.mm` | Real (T8-1b / ADR-0420) |
| `picture_import.mm` | Real (T8-IOS / ADR-0423) |
| `kernel_template.mm` | Real (T8-1b / ADR-0420) |
| `dispatch_strategy.c` | Real (ADR-0421) |

### Feature extractors (`core/src/feature/metal/`)

| Extractor | `.mm` | `.metal` | Status |
|-----------|-------|----------|--------|
| `integer_motion_v2_metal` | Yes | Yes | Real (T8-1c) |
| `float_psnr_metal` | Yes | Yes | Real (T8-1d) |
| `float_moment_metal` | Yes | Yes | Real (T8-1e) |
| `integer_psnr_metal` | Yes | Yes | Real (T8-1g) |
| `float_motion_metal` | Yes | Yes | Real (T8-1h) |
| `integer_motion_metal` | Yes | Yes | Real (T8-1i) |
| `float_ssim_metal` | Yes | Yes | Real (T8-1j) |
| `float_ms_ssim_metal` | Yes | Yes | Real (T8-2b / ADR-0490) |
| `float_ansnr_metal` | No | No | Deleted (PR #38 — ansnr feature dropped) |

All 8 active extractors have real `.mm` + `.metal` pairs compiled in `meson.build`.
No scaffold stubs remain in tree.

---

## Findings

### MT-1 — dispatch-table | severity: HIGH

**`float_ms_ssim_metal` extractor name missing from dispatch table**

`core/src/metal/dispatch_strategy.c:g_metal_features[]` lists `"float_ms_ssim"`
(the provided feature key) but not `"float_ms_ssim_metal"` (the extractor name).
Every other wired extractor has both entries: for example `"float_ssim_metal"` and
`"float_ssim"`, `"float_motion_metal"` and `"float_motion"`. The omission means
`vmaf_metal_dispatch_supports(ctx, "float_ms_ssim_metal")` returns 0, which will
cause the dispatcher to skip Metal routing for this extractor when the caller queries
by extractor name.

**Recommend:** add `"float_ms_ssim_metal"` to `g_metal_features[]` in
`core/src/metal/dispatch_strategy.c` immediately before or after the existing
`"float_ms_ssim"` entry.

---

### MT-2 — IOSurface | severity: MEDIUM

**`vmaf_metal_state_init_external` double-retains externally-supplied handles**

`core/src/metal/picture_import.mm:vmaf_metal_state_init_external` (lines ~209–217)
applies `CFRetain((__bridge CFTypeRef)device)` and then `(__bridge_retained void *)device`,
yielding a net +2 retain count on the external device. `vmaf_metal_state_free` issues
only a single `(__bridge_transfer ...)` = -1. The caller-owned +1 from `CFRetain` is
never released. The same pattern applies to an externally-supplied `command_queue`
when `queue_owned_externally == 1`.

This is an analog of the HIP pinned-host leak (PR #94): the caller expects to remain
the sole owner of its `id<MTLDevice>`, but libvmaf holds a dangling extra retain that
delays deallocation.

**Recommend:** Remove the `CFRetain` calls. `__bridge_retained` alone takes the +1
needed to anchor the handle across the C ABI boundary; the corresponding
`__bridge_transfer` in `vmaf_metal_state_free` correctly drops that single +1.
The external ownership comment in the code is the source of the confusion — the
correct interpretation is "we take our own +1; the caller still owns theirs". With
`CFRetain` absent, teardown is balanced.

---

### MT-3 — header-install | severity: MEDIUM

**`libvmaf_metal.h` Doxygen status block is stale after T8-2b**

`core/include/libvmaf/libvmaf_metal.h` lines 17–18 read:

```text
* (T8-1c/d, ADR-0421 — `integer_motion_v2.metal` + 7 additional
* feature-extractor MSL shaders) are fully shipped.
```

That describes the T8-1 batch (8 kernels). `float_ms_ssim_metal` (T8-2b / ADR-0490)
is now wired and registered, making the live count 8 shaders (after `float_ansnr` was
dropped in PR #38). The status block should reflect the current T8-2b milestone and
cite ADR-0490.

**Recommend:** Update the `@brief` status block to say "T8-1c through T8-2b,
ADR-0421 + ADR-0490 — 8 MSL shaders compiled and registered" and update the ADR
citation list.

---

### MT-4 — parity | severity: MEDIUM

**`float_ms_ssim.metal` lacks the ADR-0214 `places=4` parity gate citation**

`core/src/feature/metal/integer_motion_v2.metal` cites "Bit-exactness gate:
`places=4` cross-backend-diff against scalar (per ADR-0214)" explicitly in its
header comment. All other batch-1 kernels carry similar citations. `float_ms_ssim.metal`
(T8-2b) has no such citation and no reference to ADR-0214 in its header comment.
The parity obligation still applies per ADR-0214.

**Recommend:** Add to `float_ms_ssim.metal` header:

```text
 *  Parity gate (ADR-0214): `places=4` cross-backend-diff against CPU
 *  float_ms_ssim. Run with:
 *    scripts/parity/cross-backend-diff.sh --extractor float_ms_ssim \
 *      --ref testdata/src01_hrc00_576x324.yuv \
 *      --dis testdata/src01_hrc01_576x324.yuv \
 *      --backend metal --places 4
```

---

### MT-5 — IOSurface | severity: LOW

**`vmaf_metal_picture_import` does not validate IOSurface plane count against declared pixel format**

ADR-0423 audit requirement states the import function should validate the IOSurface
plane count and pixel format against the declared `VmafMetalConfiguration`. The
current implementation in `picture_import.mm` calls `IOSurfaceGetBaseAddressOfPlane`
and `IOSurfaceGetBytesPerRowOfPlane` directly, with no preceding call to
`IOSurfaceGetPlaneCount` or `IOSurfaceGetPixelFormat`. A YUV 4:2:2 or packed IOSurface
passed as `plane 0/1/2` would silently read out-of-bounds plane data if the surface
has fewer than 3 planes.

**Recommend:** Add a guard at the top of `vmaf_metal_picture_import`:

```objc
size_t surf_planes = IOSurfaceGetPlaneCount(surf);
if (surf_planes < 3u) {
    return -EINVAL;
}
```

This mirrors the Vulkan import guard for `VK_FORMAT` plane count validation.

---

### MT-6 — arc | severity: LOW (informational)

**AGENTS.md references `float_ansnr_metal` as "Done T8-1f" after files were deleted**

`core/src/feature/metal/AGENTS.md` lists `float_ansnr.metal` and
`float_ansnr_metal.mm` as "Done (T8-1f)" in both the wired-set inventory and the
kernel table. PR #38 deleted these files and removed the feature across all backends.
The stale table entry will mislead agents auditing wired-set completeness.

**Recommend:** Remove `float_ansnr` rows from the AGENTS.md kernel table and remove
it from the wired-set inventory sentence.

---

## Summary of audit criteria

| Criterion | Result |
|-----------|--------|
| Apple-Family-7 gate on every entry point | PASS — all `_init` / `_init_external` / `select_device_or_nil` paths gate on `MTLGPUFamilyApple7` |
| ARC correctness — no manual retain/release | PASS — all TUs use `-fobjc-arc`; no raw `[obj release]` / `[obj retain]` found |
| MTLFunction lookup nil-checks | PASS — `float_ms_ssim_metal.mm` checks all three function handles in a compound nil test; same pattern in other `.mm` files |
| MSL struct-by-value in kernel signatures | PASS — all kernels pass params as primitive MSL types (`uint4`, `float4`, `uint2`); no host-defined structs cross the buffer boundary |
| Scaffold vs real classification | PASS — all 8 live extractors have real `.mm` + `.metal` pairs; no ENOSYS stubs remain |
| Dispatch table completeness | FAIL — MT-1: `float_ms_ssim_metal` extractor name absent |
| IOSurface lifecycle correctness | FAIL — MT-2: double-retain in `init_external`; MT-5: missing plane-count guard |
| Public-header Doxygen status | FAIL — MT-3: stale kernel count after T8-2b |
| Parity gate citation | FAIL — MT-4: `float_ms_ssim.metal` missing ADR-0214 citation |
| AGENTS.md accuracy | FAIL — MT-6: stale `float_ansnr` rows |

---

## Cross-backend ULP gate command

For the `float_ms_ssim` parity run required by ADR-0214:

```bash
scripts/parity/cross-backend-diff.sh \
  --extractor float_ms_ssim \
  --ref   testdata/src01_hrc00_576x324.yuv \
  --dis   testdata/src01_hrc01_576x324.yuv \
  --backend metal --places 4
```

No code changes in this PR. All findings are recommendations only.
