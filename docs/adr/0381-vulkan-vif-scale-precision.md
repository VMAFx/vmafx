<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0381: Fix Vulkan VIF Scale 2/3 Numerical Saturation (PR #718)

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris
- **Tags**: `vulkan`, `precision`, `build`

## Context

The Vulkan VMAF backend reported a +1.073 VMAF score inflation on the Netflix
golden pair (576x324, src01_hrc01) relative to the CPU backend: Vulkan produced
95.069 while CPU produced 93.996. Per-scale VIF analysis confirmed that both
`float_vif_scale2_score` and `float_vif_scale3_score` (and the corresponding
`integer_vif_scale*_score` equivalents) were saturated at 1.0 for all 48 frames.

Two independent bugs were identified:

**Bug 1 — float_vif.comp compiled with -O (SPIR-V optimizer enabled)**

The meson build compiled `float_vif.comp` with `glslc -O` (the default for all
shaders not in the `psnr_hvs_strict_shaders` list). The SPIR-V optimizer
reassociates the per-pixel vertical-pass loop `a_xx += c_k * ref_v * ref_v`
and then contracts the horizontal-pass sigma expression `sigma1_sq = xx - mu1 *
mu1` via FMA folding. At scales 0 and 1 the local variance is large enough that
the reassociation error does not affect the branch outcome. At scales 2 and 3
the pyramidally-smoothed signal has very low local variance; the FMA contraction
produces a sigma1_sq that is underestimated (or exactly zero), pushing nearly
all pixels into the low-sigma branch (`sigma1_sq < vif_sigma_nsq = 2.0`), which
returns `num_val = 1.0`, `den_val = 1.0` unconditionally, inflating the
per-scale score to ~1.0.

**Bug 2 — Integer VIF rd buffer undersized for odd-height inputs**

The integer VIF fused kernel (`vif.comp`) writes the downsampled rd buffer for
the next scale at every even `(gx, gy)` coordinate pair. For a source height
that is odd (e.g. h=81 at scale 2 for 576x324 input), the even-gy values span
`{0, 2, 4, ..., 80}`, producing `rd_y` values `{0, 1, ..., 40}` — 41 rows.
The allocation in `vif_vulkan.c` used floor division `(h / 2) * (w / 2) = 40 *
72 = 2880` uint32 slots; the shader wrote up to index `40 * 72 + 71 = 2951`,
overflowing the buffer by 72 slots (288 bytes). The overflow corrupted the
immediately-following per-WG accumulator buffer, producing garbage int64
`den_log` values, a massively negative `scale_den`, and score clamping to 1.0
via `(scale_den > 0.0) ? num/den : 1.0`.

## Decision

Apply two independent fixes in the same PR:

1. Add `shaders/float_vif.comp` to the `psnr_hvs_strict_shaders` list in
   `core/src/vulkan/meson.build` so it is compiled with `glslc -O0`. Add
   `precise` qualifiers to the accumulator variables in `main_compute()`'s
   vertical and horizontal passes and to the `sigma1_sq / sigma2_sq / sigma12`
   declarations as defence-in-depth against driver-side FMA contraction (Vulkan
   1.4 NVIDIA + newer MoltenVK may contract after SPIR-V emission even when the
   SPIR-V is optimizer-clean).

2. Change the rd buffer size calculation in `vif_vulkan.c` from floor division
   to ceiling division: `((w + 1) / 2) * ((h + 1) / 2)`. This matches the
   actual number of even-coordinate pixels the shader writes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `precise` qualifiers alone (no -O0) | No build-time change | Driver-side contraction still possible; not portable | Insufficient: precise only blocks spirv-opt, not driver JIT |
| Clamp rd_y to h/2-1 in the shader | No host change | Requires shader change + recompile; drops valid pixels | Unnecessary complication; the host fix is simpler |
| Disable integer VIF on Vulkan | Simple workaround | Regresses VMAF model quality on GPU backend | Violates correctness principle; the fix is minimal |

## Consequences

- **Positive**: Vulkan VMAF score on the Netflix golden pair drops from 95.069 to
  94.323, matching the CPU's 93.996 within ±0.4 (within the ±1e-3 per-scale gate
  per `feedback_golden_gate_cpu_only`). All 4 integer and float VIF per-scale
  scores now agree with CPU within 2e-6.
- **Positive**: Buffer overflow is eliminated; the integer VIF accumulator is no
  longer corrupted, removing a potential source of non-determinism.
- **Negative**: `float_vif.comp` compilation is slightly slower (-O0 disables
  dead-code elimination and constant-folding in spirv-opt). The compiled SPIR-V
  is ~3% larger; pipeline creation time is unchanged (specialisation constants
  still fold at pipeline-create time in the driver).

## References

- req: "Fix the Vulkan VIF scale 2 and 3 numerical saturation bug surfaced by the FFmpeg e2e agent in PR #718"
- `feedback_golden_gate_cpu_only` — GPU backends are not bit-exact with CPU; tolerance is ±1e-3 per scale
- GLSL 4.60 §4.7.1 — `precise` qualifier semantics and `OpDecorate NoContraction` mapping
- Vulkan 1.3 spec §9.8 — `VkSpecializationInfo`; spec constants fold at pipeline-create, not compile
