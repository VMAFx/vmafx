# ADR-0437: Metal public-header install and `vmaf_metal_import_state` declaration

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: metal, build, c-api, install, apple-silicon, fork-local

## Context

Audit slice B (2026-05-15) identified two gaps that block downstream FFmpeg
`--enable-libvmaf-metal` integration:

1. **Install gap**: `libvmaf_metal.h` was absent from the `platform_specific_headers`
   list in `core/include/core/meson.build`. Every other backend header
   (`libvmaf_cuda.h`, `libvmaf_sycl.h`, `libvmaf_vulkan.h`, `libvmaf_hip.h`,
   `libvmaf_mcp.h`, `dnn.h`) has a conditional `install_headers` entry; Metal
   did not. A `meson install -Denable_metal=enabled` build therefore left the
   Metal header out of the installed tree, causing the FFmpeg configure probe
   `check_pkg_config libvmaf_metal ... libvmaf/libvmaf_metal.h
   vmaf_metal_state_init` to silently fail — the same class of failure that
   triggered ADR-0192's Vulkan guard.

2. **Declaration gap**: `vmaf_metal_import_state` (the entry point that hands a
   `VmafMetalState` to a `VmafContext`, mirroring `vmaf_cuda_import_state` /
   `vmaf_vulkan_import_state` / `vmaf_sycl_import_state`) was implemented in
   `core/src/libvmaf.c:648` but the public declaration was missing from
   `core/include/libvmaf/libvmaf_metal.h`. Without it callers could not bind
   a `VmafContext` to the Metal backend even if the header were installed.
   (Subsequent review confirmed the declaration was added to the header in an
   earlier PR before this ADR was written; the install gap was the surviving
   blocker.)

Both gaps were introduced in the original Metal scaffold (ADR-0361 / T8-1) and
survived the runtime (ADR-0420), kernel (ADR-0421), and IOSurface import
(ADR-0423) follow-ups because the install list was never updated.

The same pattern was previously fixed for Vulkan in ADR-0192, which introduced
the `is_vulkan_enabled` guard that treats both `enabled` and `auto` as "install
the header". This ADR applies the identical logic to Metal's feature option.

## Decision

We will:

1. Add a `is_metal_enabled` boolean in `core/include/core/meson.build`
   that resolves to `true` when `enable_metal` is `enabled` or `auto`, and
   conditionally append `libvmaf_metal.h` to `platform_specific_headers`.
2. Verify `vmaf_metal_import_state` is correctly declared in
   `libvmaf_metal.h` with the signature
   `int vmaf_metal_import_state(VmafContext *ctx, VmafMetalState *state)`.
3. Fix `docs/api/gpu.md` — replace the three wrong symbol names
   (`vmaf_metal_state_new` → `vmaf_metal_state_init`,
   `vmaf_metal_import_iosurface` → `vmaf_metal_picture_import`,
   `vmaf_hip_state_new` → `vmaf_hip_state_init`) and add the missing
   IOSurface sub-API table (`VmafMetalExternalHandles`,
   `vmaf_metal_state_init_external`, `vmaf_metal_picture_import`,
   `vmaf_metal_wait_compute`, `vmaf_metal_read_imported_pictures`).
4. Add a macOS-only compile+link smoke test
   `core/test/test_metal_install_header.c` that takes function-pointer
   addresses for every public Metal symbol — a type mismatch is a compile
   error, so the test catches ABI drift between header and library.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Leave Metal out of install list | No change required | Breaks FFmpeg `--enable-libvmaf-metal` configure probes; contradicts ADR-0420 "live runtime" posture | Rejected — violates ADR-0100 per-surface documentation + install contract |
| Wrap `vmaf_metal_import_state` declaration behind `#ifdef VMAF_INTERNAL` | Keeps public header minimal | Public callers cannot bind a `VmafContext` to Metal; defeats the entire API | Rejected — the function is part of the public lifecycle contract |
| Treat `enable_metal=auto` as "do not install on non-macOS" | Avoids header install on Linux where Metal is always absent | Inconsistent with Vulkan/DNN `auto` treatment; pkg-config on cross-compile hosts needs the header present | Rejected — install the header whenever the option is not `disabled`, mirroring the Vulkan guard |

## Consequences

- **Positive**: FFmpeg `--enable-libvmaf-metal` configure probes can now find
  `libvmaf_metal.h` in the install tree. Callers can bind a `VmafContext` to
  the Metal backend via `vmaf_metal_import_state`. The `docs/api/gpu.md` Metal
  section accurately reflects the live API.
- **Negative**: Adds one more installed header on macOS (negligible footprint).
- **Neutral / follow-ups**: The `test_metal_install_header` smoke test runs on
  every macOS CI build and will catch any future ABI drift between the public
  declaration and the library symbol.

## References

- Audit slice B finding §B.7 #1 (install gap) and §B.4 #2+#4 (declaration +
  docs gaps): `.workingdir/audit-2026-05-15/B-c-api-and-build.md`.
- ADR-0192 (Vulkan header install guard — precedent for feature-option install
  logic).
- ADR-0361 (Metal scaffold), ADR-0420 (Metal runtime), ADR-0423 (IOSurface
  import).
- Gap-fill plan Batch 4: `.workingdir/GAP-FILL-PLAN-2026-05-15.md`.
