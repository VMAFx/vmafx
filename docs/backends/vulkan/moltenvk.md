<!-- markdownlint-disable MD060 -->
# Vulkan via MoltenVK on macOS

> **Status (2026-05-09):** advisory CI lane — `Build — macOS Vulkan via
> MoltenVK (advisory)` — added on `macos-latest` (Apple Silicon).
> Lane is `continue-on-error: true` until one green run on `master`.
> See [ADR-0338](../../adr/0338-macos-vulkan-via-moltenvk-lane.md).

This page documents the **MoltenVK passthrough route** for running the
fork's existing Vulkan compute backend ([ADR-0127](../../adr/0127-vulkan-compute-backend.md))
on macOS without a native Metal backend.

[MoltenVK](https://github.com/KhronosGroup/MoltenVK) is the
Khronos-blessed open-source translation layer that maps Vulkan API calls
and SPIR-V shaders onto Apple's Metal API. With MoltenVK installed,
libvmaf's `enable_vulkan=enabled` build runs unchanged on Apple
Silicon — the existing GLSL-compute kernels are dispatched via Metal
under the hood.

## Why this lane exists

The fork ships four GPU paths today: CUDA (NVIDIA), SYCL (Intel +
NVIDIA), HIP (AMD), Vulkan (vendor-neutral). On macOS, none of CUDA,
SYCL, or HIP is viable (see ADR-0127 §Context). The two real options
are:

1. **Native Metal backend** — direct Metal API + per-feature MSL
   kernel ports. Best perf, but requires a parallel kernel set
   maintained alongside SPIR-V. Tracked separately.
2. **MoltenVK passthrough (this page)** — reuse the existing Vulkan
   kernels via MoltenVK's SPIR-V → MSL translator. No new kernels;
   one extra runtime dependency.

These are **complementary**, not alternatives. MoltenVK gets macOS users
GPU-accelerated VMAF on day one; the native Metal backend is the
performance-tuned path for production. The MoltenVK lane proves the
SPIR-V → MSL translation actually works end-to-end on the fork's
shaders before the Metal port lands.

## Tradeoffs vs the native Metal backend

| Axis                       | MoltenVK passthrough          | Native Metal (separate work)        |
|----------------------------|-------------------------------|-------------------------------------|
| Kernel maintenance         | Zero — reuses SPIR-V          | Parallel MSL kernel set             |
| Performance                | Translation overhead          | Direct Metal — best perf            |
| Build dependency           | `molten-vk` formula           | Apple Metal SDK (ships with Xcode)  |
| Portability of changes     | One change, all platforms     | macOS-specific kernel rewrites      |
| API surface coverage       | Limited by MoltenVK gaps      | Full Metal compute API              |
| Bug-finding granularity    | Translation layer obfuscates  | Direct stack traces                 |
| Distribution               | Bundle MoltenVK at runtime    | Standalone .dylib                   |

## Local install (Apple Silicon)

```bash
brew install molten-vk vulkan-loader vulkan-headers shaderc
```

| Formula           | Provides                                                       |
|-------------------|----------------------------------------------------------------|
| `molten-vk`       | `libMoltenVK.dylib` + `MoltenVK_icd.json` ICD descriptor       |
| `vulkan-loader`   | Khronos `libvulkan.dylib` (the loader volk dlopen()s)          |
| `vulkan-headers`  | `vulkan/vulkan.h` etc. for the build                            |
| `shaderc`         | `glslc` SPIR-V compiler                                         |

The MoltenVK ICD descriptor is installed by the `molten-vk` formula at:

```text
/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
```

Source: [Homebrew/homebrew-core `molten-vk.rb`](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb)
— the formula installs `(prefix/"etc/vulkan").install "MoltenVK/icd"
=> "icd.d"`. (Note: the path is `etc/vulkan/icd.d/`, **not**
`share/vulkan/icd.d/`. The Khronos-loader convention uses both, but
MoltenVK lays the JSON under `etc/`.)

Point the Khronos loader at MoltenVK explicitly:

```bash
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
```

Verify enumeration:

```bash
vulkaninfo --summary | head -40
# Expect: a single ICD reporting Apple GPU + driverID
# `VK_DRIVER_ID_MOLTENVK`.
```

## Building libvmaf

```bash
meson setup libvmaf core/build --buildtype release \
  -Denable_vulkan=enabled \
  -Denable_cuda=false -Denable_sycl=false
ninja -C core/build
```

The build is identical to the Linux Vulkan build — no macOS-specific
flags. `volk` (the wrap-pulled Vulkan loader) dlopen()s
`libvulkan.dylib` at runtime; the loader resolves the ICD via
`VK_ICD_FILENAMES`.

## Running the smoke test

```bash
./core/build/test/test_vulkan_smoke
./core/build/test/test_vulkan_pic_preallocation
./core/build/test/test_vulkan_async_pending_fence
```

The smoke test exercises:

- `vmaf_vulkan_device_count() >= 0` — enumeration succeeds via the
  loader → MoltenVK ICD.
- `vmaf_vulkan_context_new(&ctx, -1) == 0` — auto-pick selects the
  Apple GPU.
- NULL-safety + out-of-range device index rejection.

If `device_count == 0`, the loader is not seeing MoltenVK — check
`VK_ICD_FILENAMES` and that the brew install completed.

## Cross-backend gate (manual, advisory)

Once the smoke test passes, run the same `cross_backend_vif_diff.py`
gate the lavapipe lane uses. There is no automated gate for MoltenVK
on CI yet (the macOS lane runs the smoke test only); this is the
operator-side check.

```bash
python3 scripts/ci/cross_backend_vif_diff.py \
  --vmaf-binary core/build/tools/vmaf \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --places 4
```

Per [`feedback_no_test_weakening`](https://example.invalid) and
[CLAUDE.md §12](../../../CLAUDE.md): if a kernel fails at `places=4`
on MoltenVK, **do not lower the threshold**. Document the kernel +
the suspected MoltenVK gap in the limitations section below.

## Known limitations (MoltenVK)

The fork's Vulkan shaders rely on a small set of GLSL extensions that
don't all map cleanly onto Metal. The matrix below is sourced from
the [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
plus the fork's shader inventory.

| Shader / kernel    | Capability used                                          | MoltenVK status                                                          | Risk |
|--------------------|----------------------------------------------------------|--------------------------------------------------------------------------|------|
| `vif.comp`         | `GL_EXT_shader_explicit_arithmetic_types_int64` (non-atomic) | Maps to Metal `long` — supported on M1+                              | low  |
| `adm.comp`         | `int64` arithmetic (non-atomic), per-WG reductions       | Same as VIF — supported                                                  | low  |
| `motion.comp`      | `int64` SAD reduction (workgroup-uniform, non-atomic)    | Supported                                                                | low  |
| `motion_v2.comp`   | Same as `motion.comp`                                    | Supported                                                                | low  |
| `psnr.comp`        | `int64` per-WG reduction                                 | Supported                                                                | low  |
| `float_psnr.comp`  | `float` arithmetic only                                  | Supported                                                                | low  |
| `moment.comp`      | `GL_EXT_shader_atomic_int64` (atomicAdd on int64)        | Requires **Metal Tier-2 argument buffers** — supported on M1+ but is the most fragile dependency in the shader set | medium |
| All shaders        | SPIR-V 1.3 baseline                                      | MoltenVK targets Vulkan 1.4 with full SPIR-V 1.5/1.6 support             | low  |

General MoltenVK gaps documented in the Runtime User Guide that
**don't** currently affect the fork's kernels (listed for awareness
when porting future kernels):

- Pipeline statistics queries — not supported. (Fork doesn't use.)
- Application-controlled memory allocations passed to Vulkan are
  ignored — Metal manages allocation. (Fork uses VMA, which is
  agnostic.)
- PVRTC compressed textures — load restrictions. (Fork uses linear
  buffers, no compressed textures.)
- Vulkan validation layers require the separate Vulkan SDK loader
  install (not installed by `vulkan-loader` formula alone).

## Failure-mode playbook

When the CI lane (or your local run) reports a failure:

1. **Capture `vulkaninfo --summary`** — confirm MoltenVK is the
   loaded ICD and the device is the expected Apple GPU.
2. **Identify the failing kernel.** The smoke test contract is
   coarse-grained; a real kernel failure shows up in a feature
   extractor's per-frame score.
3. **Cross-reference the limitations table above.** If the kernel
   uses `atomicAdd` on `int64`, verify the runner GPU supports
   Metal Tier-2 argument buffers (M1 and newer; pre-M1 hardware is
   not deployed on `macos-latest`).
4. **Document the gap here.** Add a row to the table with the
   kernel + MoltenVK constraint + workaround (host-staged fallback,
   subgroup-uniform reduction without atomics, etc.).
5. **Do NOT lower the threshold or skip the case** (per
   `feedback_no_test_weakening`). The fix path is either:
   - upstream MoltenVK closes the gap, or
   - the fork rewrites the kernel pattern to avoid the constraint
     (separate work, separate ADR).

## When to flip the lane to required

Per ADR-0338, the lane stays advisory until one green run on
`master`. After that, the `continue-on-error: ${{ matrix.experimental
== true && matrix.moltenvk == true }}` line in
[`libvmaf-build-matrix.yml`](../../../.github/workflows/libvmaf-build-matrix.yml)
gets removed and the lane name is added to
[`required-aggregator.yml`](../../../.github/workflows/required-aggregator.yml).

## Relationship to the native Metal backend

This lane is **not** a replacement for the native Metal backend. The
native Metal path addresses different performance characteristics:

- No translation overhead — direct Metal compute encoder dispatches.
- Direct access to Metal-native primitives (threadgroup memory
  pinning, indirect command buffers, Metal Performance Shaders for
  reductions).
- Better stack traces when something goes wrong — no SPIR-V → MSL
  rewrite obfuscation.

MoltenVK is the **portability and cross-platform parity story**;
native Metal is the **macOS performance story**. The fork ships both:
MoltenVK remains the Vulkan-on-macOS validation path, while native
Metal owns Apple-Silicon performance work.

## References

- [ADR-0127](../../adr/0127-vulkan-compute-backend.md) — original
  Vulkan compute backend decision.
- [ADR-0338](../../adr/0338-macos-vulkan-via-moltenvk-lane.md) —
  this lane's decision record.
- [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
  — authoritative source for the limitations matrix.
- [Homebrew `molten-vk` formula](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb)
  — install layout (icd.d path).
- [Khronos Vulkan Loader specification](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderApplicationInterface.md)
  — `VK_ICD_FILENAMES` semantics.
- [Vulkan compute backend overview](overview.md) — what the SPIR-V
  shaders do and what features they cover.
