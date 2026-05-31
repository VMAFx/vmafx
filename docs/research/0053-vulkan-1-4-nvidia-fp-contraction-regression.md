<!-- markdownlint-disable MD060 -->
# Research-0053: Vulkan 1.4 bump — NVIDIA-only float contraction regression

Date: 2026-05-03
Companion ADR: [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)

## Question

Is bumping `VkApplicationInfo.apiVersion` and
`VmaAllocatorCreateInfo.vulkanApiVersion` from `VK_API_VERSION_1_3` to
`VK_API_VERSION_1_4` safe for the cross-backend bit-exactness gate, or
does it move the GPU output? An earlier exploratory bump (not on
`master`) reported two failures on NVIDIA driver 1.4.329 + RTX 4090:

- `integer_vif_scale2`: 45/48 frame mismatches, max abs `1.527e-02`.
- `ciede2000`: 42/48 frame mismatches, max abs `1.67e-04`.

Same change was clean on:

- AMD RADV driver 1.4.335 (Mesa 26.0.6, RDNA3 iGPU).
- Lavapipe (CI-side, Mesa 24.x on `ubuntu-24.04`) — predicted clean by
  symmetry with RADV, not directly measured in this digest.

The earlier agent stopped at the symptom, correctly per the
`no-test-weakening` rule. This digest takes the investigation to root
cause and decides the path forward.

## Approach

1. Identify the two GLSL shaders implicated.
2. Confirm whether the SPIR-V bytecode the build emits actually changes
   when only the runtime API request changes (it shouldn't — the
   shader compile target is independent of the instance API).
3. Inspect the SPIR-V execution-mode block for explicit
   float-controls decorations.
4. Cross-reference with NVIDIA's `shaderFloatControls2` exposure in
   `vulkaninfo`.
5. Prototype the SPIR-V-side mitigations (precise / NoContraction /
   `OpExecutionMode ContractionOff`) and report what is reachable from
   GLSL today.
6. Decide between (a) shipping the bump with shader-side mitigations,
   (b) shipping docs-only and deferring the bump as a tracked backlog
   item.

## Findings

### 1. Implicated shaders

| Failing feature | Shader file | Workgroup geometry | Float math hot path |
|---|---|---|---|
| `integer_vif_scale2` | [`core/src/feature/vulkan/shaders/vif.comp`](../../core/src/feature/vulkan/shaders/vif.comp) | 32 × 4 | `g = sigma12 / sigma1_sq`, `sv_sq = sigma2_sq − g·sigma12`, `gg = g·g·sigma1_sq` (lines 498–503) — three FMA-reorderable expressions on float32 |
| `ciede2000` | [`core/src/feature/vulkan/shaders/ciede.comp`](../../core/src/feature/vulkan/shaders/ciede.comp) | 16 × 8 | yuv→rgb 3×3 mat-mul, sRGB `pow`, xyz→Lab cube root, ciede2000 chained `pow`/`sqrt`/`sin`/`cos`/`atan` (lines 132–260) — entire per-pixel chain is float32 with no `precise` qualifiers |

Both shaders run scalar float32 throughout. Neither uses
`shaderFloat16` or any subgroup FP reduction; the FP math is
per-thread.

### 2. SPIR-V is byte-identical between target-env vulkan1.3 and 1.4

The fork's [`core/src/vulkan/meson.build`](../../core/src/vulkan/meson.build)
hardcodes `glslc --target-env=vulkan1.3` (line 106). The hypothetical
1.4 bump only touches `VkApplicationInfo.apiVersion` and
`VmaAllocatorCreateInfo.vulkanApiVersion` — not the shader compile
target. To rule out an indirect bytecode change, both shaders were
compiled at both target levels and compared:

```text
glslc --target-env=vulkan1.3 -O vif.comp   -o vif-13.spv
glslc --target-env=vulkan1.4 -O vif.comp   -o vif-14.spv
glslc --target-env=vulkan1.3 -O ciede.comp -o ciede-13.spv
glslc --target-env=vulkan1.4 -O ciede.comp -o ciede-14.spv
cmp vif-13.spv  vif-14.spv    # identical (28 180 bytes)
cmp ciede-13.spv ciede-14.spv # identical (16 412 bytes)
```

Both pairs are bit-identical at glslc 2026.1. The regression therefore
**cannot be a build-side codegen shift** — it is entirely runtime
shader-compiler behaviour on the NVIDIA proprietary driver.

### 3. SPIR-V emits no float-controls execution modes

`spirv-dis` of either shader shows only `OpExecutionMode <main>
LocalSize <x> <y> 1` — no `RoundingModeRTE…`, no `DenormPreserve…`, no
`SignedZeroInfNanPreserve…`, no per-result `NoContraction`
decorations. The shader makes no precision contract with the driver,
so the driver picks defaults.

### 4. NVIDIA `shaderFloatControls2` is exposed and float32 denorms are flushed

`vulkaninfo` on NVIDIA RTX 4090 + driver 595.71.5.0 (Vulkan
1.4.329) reports:

| Property | Value |
|---|---|
| `apiVersion` | `1.4.329` |
| `VK_KHR_shader_float_controls2` | revision 1 (core in 1.4) |
| `shaderFloatControls2` | `true` |
| `shaderDenormPreserveFloat32` | `false` |
| `shaderDenormFlushToZeroFloat32` | `false` |
| `shaderRoundingModeRTEFloat32` | `true` |
| `shaderRoundingModeRTZFloat32` | `true` |
| `shaderSignedZeroInfNanPreserveFloat32` | `true` |

RADV on the same machine (driver 26.0.6, Vulkan 1.4.335) reports
**both** `Preserve` and `FlushToZero` for float32 — i.e. its NIR
backend honours whatever execution mode the SPIR-V declares, and the
absence of one keeps the more conservative path. NVIDIA reports
neither preserve nor flush as guaranteed — its compiler is free to
pick per-build.

The `shaderFloatControls2` capability is the relevant 1.4 promotion.
Until 1.4 it was an optional extension; in 1.4 it is core, which means
NVIDIA's compiler activates the v2 float-controls codegen path
unconditionally when an app declares 1.4. The v1 → v2 change in NVIDIA's
shader compiler appears to have **changed the default FMA-contraction
policy** for the fork's shaders, which shaderFloatControls2 was
specifically designed to expose. Without an `OpExecutionMode`
declaring intent, the policy is "implementation-defined" per spec.

### 5. The only Vulkan-side knob on FMA is `OpDecorate <result> NoContraction`

Vulkan SPIR-V does not allow `OpExecutionMode ... ContractionOff`
(prototyped — `glslc` rejects with "ContractionOff requires Kernel
capability"). That mode is OpenCL-only.

The Vulkan-supported equivalent is per-result, via the
[`NoContraction` decoration](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpDecorate)
which GLSL emits when the result is a `precise`-qualified float:

```glsl
precise float r = a * c + d;   // OpDecorate %r NoContraction
```

A prototype confirmed `glslc` lowers `precise` to `OpDecorate %29
NoContraction` per arithmetic op. There is no module-wide hammer; the
load-bearing FP ops have to be tagged individually.

### 6. Quantitative size of the regression

The reported max abs deltas are well above the `places=4`
cross-backend gate ([ADR-0214](../adr/0214-gpu-parity-ci-gate.md)):

- `integer_vif_scale2 = 1.527e-02` ≫ `5.0e-05` (`places=4` threshold).
- `ciede2000 = 1.67e-04` ≫ `5.0e-05`.

Per-feature denominators put the relative error at:

- VIF scale 2: `gg_sigma_f` is on the order of `1e3..1e4`, so
  `1.5e-2` is `~1 ulp`-class drift on the inner expression that
  cascades into the per-frame VIF score (sum-of-logs over the plane).
  Single-frame drift of 1.5e-2 in the integrand is at the edge of
  `places=4` but the cross-backend gate cumulates across frames.
- ciede2000: per-pixel ΔE is `O(10)`, so `1.67e-4` is ~5 ULP of
  float32 — consistent with one extra FMA contraction inside the
  chained `pow`/`sqrt` chain.

Both are exactly the magnitude an FMA-fold-vs-no-fold flip produces
on these expressions.

### Why RADV stays clean

Mesa NIR's float-controls handling is **conservative**: the absence of
a declared execution mode is read as "no permission to contract". RADV
therefore keeps `a*b + c` as separate `OpFMul` + `OpFAdd` and matches
the CPU bit-for-bit. NVIDIA's compiler at 1.3 effectively did the same
thing; at 1.4 it appears to have flipped the default in the absence of
a declared mode.

### Why lavapipe (Mesa 24.x) is expected clean

Lavapipe is a software rasterizer; its float ops are scalar host
arithmetic with no FMA fast path. It cannot synthesise an FMA at any
API level, so there is no contraction available to flip on. Direct
measurement is left to the CI gate run against the bumped branch.

## Decision

**Defer the 1.4 API-version bump.** Track as a backlog item gated on a
two-step shader-side fix:

1. **Step A** — Audit `vif.comp` and `ciede.comp` for the load-bearing
   FP expressions (the three lines in VIF and the chained per-pixel
   math in CIEDE) and tag them `precise` so glslc emits
   `OpDecorate ... NoContraction`. Re-build, re-disassemble, confirm
   the SPIR-V now declares the contract. Re-run the cross-backend
   gate at `apiVersion = 1.4` against NVIDIA + RADV + lavapipe.
2. **Step B** — Once step A is clean, bump the three sites in
   `core/src/vulkan/common.c` (`apiVersion = VK_API_VERSION_1_4` at
   line 54 + 264 + 374) and the `VMA_VULKAN_VERSION` define in
   `core/src/vulkan/vma_impl.cpp` (line 22) to `1004000`. Re-run
   the gate. Land in one PR with the digest cross-link.

The fork has no current need for any 1.4-promoted feature
(`VK_KHR_dynamic_rendering_local_read`,
`VK_KHR_maintenance5/6/7`, `VK_KHR_push_descriptor` — all
graphics-pipeline conveniences not used by the compute-only kernel
template) so the bump is exploratory. Deferring it costs nothing
operational.

This investigation explicitly rules out:

- "Bump and lower the gate" — violates the no-test-weakening rule.
- "Bump and skip NVIDIA in CI" — violates the no-skip-shortcuts rule
  (and the CI gate doesn't run NVIDIA anyway; lawrence's local NVIDIA
  GPU is the only NVIDIA validation lane the fork has).
- "Regen the GPU snapshot to match 1.4 NVIDIA output" — violates the
  Netflix golden gate's spirit (CPU is ground truth) and bakes a
  driver bug into the fork's snapshot ledger.

## Reproduction

The investigation is bytecode-only and reproducible from any
worktree:

```sh
glslc --target-env=vulkan1.3 -O \
  core/src/feature/vulkan/shaders/vif.comp -o /tmp/vif-13.spv
glslc --target-env=vulkan1.4 -O \
  core/src/feature/vulkan/shaders/vif.comp -o /tmp/vif-14.spv
cmp /tmp/vif-13.spv /tmp/vif-14.spv      # identical
spirv-dis /tmp/vif-13.spv | grep -E 'ExecutionMode|NoContraction'
# Only emits: OpExecutionMode %main LocalSize 32 4 1
vulkaninfo | grep -E 'shaderFloatControls2|shaderRoundingMode|shaderDenorm'
```

The runtime regression itself requires NVIDIA driver ≥ 1.4.329 and a
locally-applied 1.4 bump on the four sites listed above; the gate
command is the standard `/cross-backend-diff` skill against the
Netflix normal pair (`src01_hrc00_576x324.yuv` vs
`src01_hrc01_576x324.yuv`).

## Open questions

- Does the NVIDIA driver release notes confirm a 1.4 codegen-default
  change? (Filed as a follow-up — the user-visible Vulkan release
  notes for 595.x are sparse on shader-compiler internals.)
- Is the same regression visible on `psnr_hvs`, `ssimulacra2_xyb`, or
  any other float-heavy shader the fork ships? (The two reported
  failures are the only ones the earlier agent measured. Step A's
  audit is a natural place to broaden the sweep.)

## References

- [SPIR-V 1.6 — `OpDecorate NoContraction`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpDecorate)
- [SPIR-V 1.6 — Execution Mode `ContractionOff` (Kernel-only)](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpExecutionMode)
- [`VK_KHR_shader_float_controls2`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_shader_float_controls2.html)
  (promoted to core in 1.4)
- [`VK_KHR_shader_float_controls`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_shader_float_controls.html)
- [GLSL `precise` qualifier — GLSL 4.20+](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.50.pdf#section.4.7.1)
- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend parity gate.
- [ADR-0187](../adr/0187-ciede-vulkan.md) — ciede2000 Vulkan port
  (precision contract).
- Existing in-tree precedent for FMA-reordering mitigations:
  [`core/src/vulkan/meson.build`](../../core/src/vulkan/meson.build)
  lines 80–99 — the `psnr_hvs_strict_shaders` `-O0` list (`ssimulacra2_blur`,
  `ssimulacra2_xyb`, `ssimulacra2_ssim`) already documents this class
  of issue at the build level.
- Source: `req` (parent-agent investigation request, 2026-05-03).
