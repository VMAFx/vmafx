<!-- markdownlint-disable MD060 -->
# Research-0089: MoltenVK feasibility on the fork's Vulkan shader inventory

- **Date**: 2026-05-09
- **Author**: Lusoris, Claude (Anthropic)
- **Companion ADR**: [ADR-0338](../adr/0338-macos-vulkan-via-moltenvk-lane.md)

## Question

Will the fork's existing Vulkan compute kernels (per
[ADR-0127](../adr/0127-vulkan-compute-backend.md) and the kernel ADRs
0176–0252) run end-to-end on Apple Silicon via MoltenVK, the
Khronos-supported open-source Vulkan-on-Metal translation layer?
And if not, which kernels are at risk and why?

## TL;DR

**Probably yes for 6 of 7 shaders; medium risk on `moment.comp`.**
The fork's Vulkan shaders rely on a small, well-defined set of GLSL
extensions. The non-atomic `int64`-arithmetic path lowers to Metal's
native `long` type and is supported on M1+. The single shader
(`moment.comp`) using `atomicAdd` on `int64`
(`GL_EXT_shader_atomic_int64`) requires Metal Tier-2 argument
buffers per the MoltenVK Runtime User Guide — supported on M1+ but
the most fragile capability dependency. Any failure on the macOS
runner is most likely to surface there first.

## Method

1. Inventory the fork's Vulkan shaders under
   [`core/src/feature/vulkan/shaders/`](../../core/src/feature/vulkan/shaders/)
   and grep for GLSL extension `#extension` directives + atomic
   intrinsics.
2. Cross-reference each capability against the
   [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
   — the authoritative source for known translation-layer gaps.
3. Verify Apple Silicon GPU capability tiers (`macos-latest` is
   Apple Silicon; M1+ supports Tier-2 argument buffers).
4. Validate Homebrew install layout from the
   [Homebrew/homebrew-core `molten-vk.rb`](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb)
   formula source.

## Shader inventory + capability matrix

| Shader            | Extensions used                                          | Atomic ops?                | MoltenVK risk |
|-------------------|----------------------------------------------------------|----------------------------|---------------|
| `vif.comp`        | `int64` arithmetic (non-atomic)                          | None                       | low           |
| `adm.comp`        | `int64` arithmetic (non-atomic)                          | None                       | low           |
| `motion.comp`     | `int64` arithmetic, per-WG SAD reduction                 | None (subgroup-uniform)    | low           |
| `motion_v2.comp`  | `int64` arithmetic                                       | None                       | low           |
| `psnr.comp`       | `int64` per-WG reduction                                 | None                       | low           |
| `float_psnr.comp` | `float` only                                             | None                       | low           |
| `moment.comp`     | `GL_EXT_shader_atomic_int64`                             | `atomicAdd` on `int64`     | **medium**    |

(The `cambi_*.comp` shaders are not in this matrix because they use
only `int32` arithmetic and `int32` shared memory — entirely
within MoltenVK's vanilla Vulkan 1.0 surface.)

## Capability deep-dive

### `int64` arithmetic (non-atomic)

`GL_EXT_shader_explicit_arithmetic_types_int64` lowers SPIR-V
`OpTypeInt 64 1` operations to Metal's `long` type. MoltenVK's
SPIRV-Cross handles this without special configuration. Apple GPUs
have native 64-bit integer support since the M1; no driver
configuration required.

**Conclusion**: low risk. Six of the seven listed shaders fall
entirely in this bucket.

### `atomicAdd` on `int64` (`moment.comp`)

`GL_EXT_shader_atomic_int64` requires the
[`VK_KHR_shader_atomic_int64`](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#VK_KHR_shader_atomic_int64)
Vulkan extension. The MoltenVK Runtime User Guide notes this
extension as supported with the constraint:

> Requires GPU Tier 2 argument buffers support.

Apple Silicon (M1, M1 Pro/Max/Ultra, M2, M3, …) supports
Tier-2 argument buffers per
[Apple's Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf).
GHA `macos-latest` runs on Apple Silicon (Homebrew prefix
`/opt/homebrew` confirms M-series), so the capability is present.

The risk surface is **driver maturity**, not capability presence:
SPIRV-Cross's int64-atomic translation has historically been a
maturity hot-spot, and a regression in a future MoltenVK release
could break this shader without breaking the others. The advisory
lane catches that class of regression.

**Conclusion**: medium risk. The capability is present but the
translation path has historically been the most fragile.

### `VK_KHR_external_memory_fd` (DMABUF import)

The fork's Vulkan runtime uses DMABUF import for FFmpeg hw-frame
zero-copy (per ADR-0127 §Decision and ADR-0186). MoltenVK does
**not** support `VK_KHR_external_memory_fd` — Apple's IOSurface
import path is the Metal-native equivalent and is not Vulkan-
exposed.

**Mitigation**: ADR-0127 already specifies that on MoltenVK the
external-memory path falls back to a host-staged copy. The smoke
tests **do not** exercise import; they allocate via VMA host-
visible buffers, which works.

**Conclusion**: not a blocker for the smoke-test lane. Becomes
relevant once the fork wants zero-copy ffmpeg-on-macOS.

### Pipeline statistics queries / app-controlled allocations / PVRTC

Per the MoltenVK User Guide these are documented gaps. None apply
to the fork's shaders or runtime use:

- Pipeline statistics: not used.
- App-controlled allocations: VMA is allocation-policy-agnostic.
- PVRTC: no compressed textures.

## Install layout (verified)

From
[homebrew-core `molten-vk.rb`](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb):

```ruby
inreplace "MoltenVK/icd/MoltenVK_icd.json",
          "./libMoltenVK.dylib",
          (lib/"libMoltenVK.dylib").relative_path_from(prefix/"etc/vulkan/icd.d")
(prefix/"etc/vulkan").install "MoltenVK/icd" => "icd.d"
```

Resolved on Apple Silicon (`/opt/homebrew` prefix):

- ICD descriptor: `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`
- MoltenVK dylib: `/opt/homebrew/lib/libMoltenVK.dylib`
- Loader (separate formula `vulkan-loader`): `/opt/homebrew/lib/libvulkan.dylib`
- Headers (`vulkan-headers`): `/opt/homebrew/include/vulkan/`
- glslc (`shaderc`): `/opt/homebrew/bin/glslc`

The Khronos loader looks for ICDs at compile-time defaults plus
`VK_ICD_FILENAMES`; pinning the env var to MoltenVK's JSON path
makes the lane deterministic regardless of which other ICDs the
runner image has.

## Expected lane wall-clock cost

Estimate based on the current `Build — macOS clang (CPU)` lane
(the only existing macos-latest reference) plus brew-install
overhead for the Vulkan stack:

| Step                                                | Estimate     |
|-----------------------------------------------------|--------------|
| Checkout + python + meson install                   | ~30s         |
| `brew install` ninja, nasm, ccache, llvm, pkg-config | ~60s         |
| `brew install` molten-vk, vulkan-loader, vulkan-headers, shaderc | ~90s         |
| `meson setup` + `ninja` (Vulkan build)              | ~6–9 min     |
| Smoke tests (3 binaries)                            | ~10s         |
| **Total**                                           | **~9–12 min**|

Well within the 60-min `macos-latest` runner timeout. Cost is
roughly equivalent to the existing macOS CPU+DNN lane.

## Why advisory, not required

Per ADR-0338:

1. MoltenVK is a moving target. A fresh-install brew bump can
   surface a regression in SPIR-V → MSL translation that breaks the
   atomic-int64 path; a required lane would block every PR until
   upstream MoltenVK fixes it (out of our control).
2. The lane is paid for by `macos-latest` minutes; a flaky required
   lane burns budget.
3. Mirrors the precedent set by ADR-0127's Arc-A380 nightly lane
   (advisory until self-hosted runner registered).

The promotion criterion is concrete: **one green run on `master`**
flips `continue-on-error` off and adds the job name to
`required-aggregator.yml`.

## Open questions

- **MoltenVK release cadence vs the fork's PR cadence** —
  unanswered. If the bump-and-break cycle is faster than our PR
  cycle, the lane oscillates. Tracked under
  [docs/state.md](../state.md) as a future watch item.
- **Native Metal backend overlap** — the parallel scaffold lands
  separately. Once both exist, this digest's relevance is bounded
  to the period before the Metal backend matures.

## References

- [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
- [Homebrew `molten-vk` formula](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb)
- [`VK_KHR_shader_atomic_int64`](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#VK_KHR_shader_atomic_int64)
- [Apple Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [GitHub Actions macOS runner spec](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md)
- Fork shader inventory:
  [`core/src/feature/vulkan/shaders/`](../../core/src/feature/vulkan/shaders/)
- [ADR-0127](../adr/0127-vulkan-compute-backend.md) — Vulkan
  compute backend.
- [ADR-0338](../adr/0338-macos-vulkan-via-moltenvk-lane.md) —
  the lane this digest informs.
