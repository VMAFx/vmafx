<!-- markdownlint-disable MD060 -->
# ADR-0264: Vulkan 1.4 API-version bump blocked on shader FP-contraction audit

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude
- **Tags**: vulkan, fork-local, bit-exactness, backlog, docs

## Context

An exploratory bump of `VkApplicationInfo.apiVersion` and
`VmaAllocatorCreateInfo.vulkanApiVersion` from `VK_API_VERSION_1_3` to
`VK_API_VERSION_1_4` (four sites total in
[`core/src/vulkan/common.c`](../../core/src/vulkan/common.c) +
[`core/src/vulkan/vma_impl.cpp`](../../core/src/vulkan/vma_impl.cpp))
moves NVIDIA's GPU output for two compute kernels above the
`places=4` cross-backend gate ([ADR-0214](0214-gpu-parity-ci-gate.md)):

- `integer_vif_scale2` — 45/48 frames mismatch, max abs `1.527e-02`.
- `ciede2000` — 42/48 frames mismatch, max abs `1.67e-04`.

The same change is clean on AMD RADV (Mesa 26.0.6) and predicted clean
on lavapipe (no FMA fast path).

The investigation in
[research-0053](../research/0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
proves that:

1. The compiled SPIR-V is byte-identical at `--target-env=vulkan1.3`
   and `vulkan1.4` for both shaders — the build does not change.
2. Neither `vif.comp` nor `ciede.comp` declares any float-controls
   execution mode or `precise`/`NoContraction` decoration.
3. NVIDIA driver 595.x exposes core-1.4 `shaderFloatControls2` and
   does **not** guarantee `shaderDenormPreserveFloat32` /
   `shaderDenormFlushToZeroFloat32` — its compiler is free to pick
   per-build, and the 1.3→1.4 transition appears to flip the default
   FMA-contraction policy for these shaders.
4. The only Vulkan-side knob that constrains FMA contraction is
   per-result `OpDecorate ... NoContraction` (emitted by GLSL
   `precise`); SPIR-V `ContractionOff` execution mode is OpenCL-only
   and rejected by Vulkan.

The fork has no in-flight requirement for any 1.4-promoted Vulkan
feature (`VK_KHR_dynamic_rendering_local_read`,
`VK_KHR_maintenance5/6/7`, `VK_KHR_push_descriptor`,
`VK_KHR_zero_initialize_workgroup_memory`,
`VK_KHR_shader_subgroup_uniform_control_flow`) — the fork's compute
path uses none of them. The bump is exploratory.

## Decision

We will **not bump `apiVersion` to `VK_API_VERSION_1_4` until the
shader-side FP-contraction audit lands**. The bump is tracked as
backlog item **T-VK-1.4-BUMP** in two steps:

1. **Step A (must precede the bump)** — Audit `vif.comp` and
   `ciede.comp` (and any other compute shader the cross-backend gate
   surfaces under a 1.4 NVIDIA run) and tag the load-bearing FP
   expressions `precise`. The minimum scope is the three lines around
   `vif.comp:498-503` (`g`, `sv_sq`, `gg_sigma_f`) and the chained
   per-pixel math in `ciede.comp:132-260`. Re-disassemble with
   `spirv-dis` to confirm `OpDecorate ... NoContraction` is present.
   Re-run `/cross-backend-diff` against NVIDIA + RADV + lavapipe at
   `places=4`.
2. **Step B (only after Step A is clean on all three drivers)** —
   Bump the three `apiVersion = VK_API_VERSION_1_3` sites in
   `core/src/vulkan/common.c` (lines 54, 264, 374) and the
   `VMA_VULKAN_VERSION` define in `core/src/vulkan/vma_impl.cpp`
   (line 22, `1003000` → `1004000`). Re-run the gate.

Until both steps land, `master` stays on `VK_API_VERSION_1_3`. Lowering
the gate threshold or skipping the NVIDIA validation lane is
explicitly rejected — see *Alternatives considered*.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Defer + audit + bump (chosen)** | Honours `places=4` gate; bit-exact on all measured drivers post-fix; matches the existing `psnr_hvs_strict_shaders` precedent in [`core/src/vulkan/meson.build`](../../core/src/vulkan/meson.build); zero operational cost (no feature requires 1.4 today) | Defers the bump indefinitely if the audit slips | Highest-quality outcome; aligns with no-test-weakening rule |
| Bump now and lower the cross-backend gate to `places=3` | Unblocks the API bump immediately | Violates [the no-test-weakening rule](../../CLAUDE.md) and [ADR-0214](0214-gpu-parity-ci-gate.md) — the gate exists precisely to catch this class of drift | Rejected on principle |
| Bump now and gate NVIDIA out of the cross-backend run | Unblocks for lavapipe + RADV CI | Violates the no-skip-shortcuts rule; lawrence's local NVIDIA GPU is the only NVIDIA validation lane; CI doesn't run NVIDIA today so the "fix" is illusory | Rejected — turns a known regression into invisible debt |
| Bump now and regenerate the GPU snapshot at 1.4 NVIDIA output | One-line change | Bakes the driver-side codegen flip into the fork's snapshot ledger; CPU is ground truth per [§8 of CLAUDE.md](../../CLAUDE.md) — GPU snapshots track CPU, not their own driver-current behaviour | Rejected — wrong direction for a numerical fork |
| Tag `precise` everywhere unconditionally (regardless of bump) | Belt-and-braces bit-exactness across drivers | Loses the FMA fast path on every shader, even where it's load-bearing for perf and the contraction is harmless (e.g. ciede's matmul is FMA-friendly on driver paths that don't reorder destructively) | Out of scope here; can be a follow-up if the audit is too narrow |

## Consequences

- **Positive**:
  - `master` stays bit-exact on the cross-backend gate at the current
    Vulkan 1.3 baseline; no regression shipped.
  - The investigation is captured in
    [research-0053](../research/0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
    so the next person who reaches for the bump finds the prior art.
  - The `precise` audit, when it lands, hardens the shaders against
    *future* driver codegen changes (the same class of bug could
    surface on a future RADV release that flips its NIR default).

- **Negative**:
  - The fork cannot use any 1.4-promoted feature until the audit
    completes. None are needed today; this becomes a real cost only
    when one is.

- **Neutral / follow-ups**:
  - Backlog item **T-VK-1.4-BUMP** added to
    [`docs/state.md`](../state.md) Deferred section.
  - Follow-up audit may surface additional float-heavy shaders
    (`psnr_hvs.comp`, `ssimulacra2_xyb.comp`, `ssimulacra2_blur.comp`,
    `ssimulacra2_ssim.comp`) that need the same treatment. The last
    three already carry an `-O0` workaround for an FMA-reordering
    issue at the build level — the `precise` audit can subsume some
    of those.
  - File a downstream NVIDIA report (driver feedback) with the
    minimal repro once the audit fix is shipped, asking whether the
    1.3 vs 1.4 default-codegen flip is intentional. Not a blocker.

## References

- [research-0053](../research/0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
  — root-cause investigation digest.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend parity gate.
- [ADR-0187](0187-ciede-vulkan.md) — ciede2000 Vulkan port +
  precision contract.
- [`core/src/feature/vulkan/shaders/vif.comp`](../../core/src/feature/vulkan/shaders/vif.comp)
  — integer VIF compute shader.
- [`core/src/feature/vulkan/shaders/ciede.comp`](../../core/src/feature/vulkan/shaders/ciede.comp)
  — ciede2000 compute shader.
- [`core/src/vulkan/common.c`](../../core/src/vulkan/common.c)
  — three `apiVersion` sites.
- [`core/src/vulkan/vma_impl.cpp`](../../core/src/vulkan/vma_impl.cpp)
  — `VMA_VULKAN_VERSION` define.
- Source: `req` (parent-agent investigation request, 2026-05-03):
  paraphrased — *bumping `VK_API_VERSION_1_3` → `VK_API_VERSION_1_4`
  causes a bit-exactness regression on NVIDIA driver 1.4.329 for
  `integer_vif_scale2` (45/48 mismatches, max abs 1.527e-02) and
  `ciede2000` (42/48 mismatches, max abs 1.67e-04); investigate the
  root cause, decide fix-vs-document, ship accordingly.*

## Status update 2026-05-09: Step B applied, gated on Phase 3c

The four `apiVersion` sites have been bumped from `VK_API_VERSION_1_3`
to `VK_API_VERSION_1_4` (`common.c:54`, `:264`, `:374`;
`vma_impl.cpp:22` `1003000` → `1004000`). The bump ships as a DRAFT PR
held behind Phase 3c (PR #512, NVIDIA `subgroupAdd(int64_t)`
workaround). Cross-backend parity gate at API 1.4, this session,
against `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv` (48
frames, places=4):

| Device                                                 | vif          | ciede        | adm | motion | psnr |
| ------------------------------------------------------ | ------------ | ------------ | --- | ------ | ---- |
| NVIDIA RTX 4090 (driver 595.71.05, Vulkan 1.4.329)     | FAIL 45/48   | OK (8.9e-05) | OK  | OK     | OK   |
| Intel Arc A380 (Mesa anv, Vulkan 1.4.348)              | OK (2.0e-06) | OK (6.9e-05) | OK  | OK     | OK   |
| AMD RADV (lavapipe RAPHAEL\_MENDOCINO, Vulkan 1.4.348) | OK (2.0e-06) | OK (8.3e-05) | OK  | OK     | OK   |

NVIDIA `integer_vif_scale2` max abs is 1.527e-02 — identical to the
original Step-B-blocked baseline, confirming Phase 3c is the only
remaining blocker. Step B's PR is block-on-merge until Phase 3c lands
and all three lanes report 0/N mismatches.
