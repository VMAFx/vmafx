# ADR-0178: Vulkan ADM kernel (T5-1c-adm)

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `gpu`, `feature-extractor`, `numerical-correctness`

## Context

T5-1c extends the Vulkan kernel matrix beyond VIF to ADM, motion, and
motion_v2. PR #119 (commit `32e31e45`) landed the motion kernel; this
ADR covers the **ADM** half of T5-1c (the wavelet-based kernel that's
the largest and most complex of the three).

The CPU integer-ADM extractor at `core/src/feature/integer_adm.c`
(3527 LOC) implements a 4-scale DWT (CDF 9/7 / DB2 wavelet) followed
by per-band decoupling, CSF weighting, and contrast masking. The SYCL
implementation at `integer_adm_sycl.cpp` (1626 LOC) was the closest
existing GPU port; the Vulkan kernel mirrors its 5-stage pipeline:

1. DWT vertical pass.
2. DWT horizontal pass (yields LL, LH, HL, HH per scale).
3. Decouple + CSF fused.
4. CSF denominator reduction.
5. Contrast measure reduction.

Run for each of 4 scales = 20 stage executions per frame. The shader
uses one pipeline per `(scale, stage)` pair via `VkSpecializationInfo`
(16 pipelines after merging stages 4+5 into a single fused reduction
pass, matching the SYCL pattern).

## Decision

We will land a Vulkan compute kernel for `integer_adm` that produces
the standard `integer_adm2`, `integer_adm_scale0..3` outputs (plus the
underlying num/den intermediates and debug features when `debug=true`),
gated against the CPU scalar reference by a third lavapipe step inside
the existing `Vulkan VIF Cross-Backend (lavapipe, places=4)` lane.
The Arc nightly lane gets the matching step.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| One pipeline per (scale, stage), 16 total | Clear flow; matches SYCL structure | More pipelines to manage | Adopted; matches the structure proven on VIF (4 scales × 1 stage = 4 pipelines) |
| One mega-shader with per-stage `STAGE` push constant | Fewer pipelines to compile | Larger shader; harder to verify per-stage correctness | The 16-pipeline split is what the SYCL reference uses; symmetry simplifies the diff |
| Defer ADM to a follow-up PR | Smaller motion PR review | Two more weeks of partial Vulkan coverage; T5-1c stays open | User answered "Two PRs: motion + motion_v2 first, ADM second" — this is PR 2 of 2 |
| Pure-`int64` reductions across all scales | Maximum bit-exactness | Some host-side double-precision finalisation matches CPU exactly already (reduction-order doesn't affect places=4) | Hybrid `int64` partials + host double sum is what SYCL does and what motion/vif do; matches CPU at `places=4` with small (≤3.1e-5) residuals on Arc |

The empirical baseline against the CPU scalar reference, on the
Netflix normal pair (48 frames at 576×324) AND the 1920×1080
checkerboard pair (3 frames), measured on **Intel Arc A380 with
the corrected gate** (see "Bug history" below):

| metric | max_abs_diff | places=4 mismatches |
| --- | --- | --- |
| `integer_adm2` | 5e-6 | 0/48 ✓ |
| `integer_adm_scale0` | 0.0 (bit-exact) | 0/48 ✓ |
| `integer_adm_scale1` | 3.1e-5 | 0/48 ✓ |
| `integer_adm_scale2` | 1e-6 | 0/48 ✓ |
| `integer_adm_scale3` | 1e-6 | 0/48 ✓ |

NVIDIA RTX 4090 via the proprietary Vulkan driver shows a larger
~2.4e-4 residual on `adm_scale2` (1/48 frame mismatch at `places=4`).
The kernel is identical; the divergence is attributed to NVIDIA's
Vulkan driver doing the host-side reductions in a different summation
order than Mesa anv. This is tracked as a separate kernel-side fix
(host-side reduction → Kahan summation or replicated CPU loop order).

## Bug history (this PR exposes the cross-backend gate as broken)

The "claimed ULP=0" line in earlier ADRs (0176, 0177) and CHANGELOG
entries was bogus. PR #120's investigation found three compounding
bugs:

1. **`tools/meson.build` never set `-DHAVE_VULKAN=1`.** PR #118 added
   the `--vulkan_device` CLI block under `#ifdef HAVE_VULKAN` to
   `vmaf.c` but missed the meson plumbing. Result: every `--vulkan_device`
   invocation since #118 silently no-op'd; the binary ran CPU. CI's
   lavapipe lane was passing CPU-vs-CPU (trivially ULP=0).
2. **`vmaf_use_feature()` skipped `set_fex_vulkan_state()`.** When the
   user passed `--feature vif_vulkan --vulkan_device 1` (the gate
   path), the framework registered the extractor but never propagated
   the imported `VmafVulkanState`. The lazy fallback then auto-picked
   device 0 (RTX 4090), so what looked like "Arc verification" was
   actually NVIDIA verification.
3. **`scripts/ci/cross_backend_vif_diff.py` invoked `--feature X`
   for both sides.** Without `--no_prediction`, the default model
   loaded the CPU extractor alongside the Vulkan one; both registered
   for the same feature names and the second writer's score was
   silently dropped with a "cannot be overwritten" warning.

The fix bundle in PR #120's commit history (`6167e300`, `de65c0ac`,
`2aa79db1`) closes all three. After the fix the gate honestly
compares CPU vs the named GPU extractor on the chosen device, with
absolute-tolerance comparison (replaces the brittle `round() != round()`
banker-rounding boundary).

## Consequences

- **Positive**: Vulkan kernel matrix matches the SYCL/CUDA kernel sets
  for the production VMAF model (VIF + motion + ADM = the three
  features the default `vmaf_v0.6.1` model consumes). T5-1c closes.
  The cross-backend gate now covers all three features on every PR
  AND honestly exercises the GPU path (post-fix).
- **Negative**: 16 pipelines per ADM extractor = longer init time
  than VIF's 4. Acceptable since init runs once per `vmaf_init` call.
  The shader is the largest in the fork (~660 LOC GLSL); future Mesa
  Vulkan compiler bumps will recompile longer.
- **Neutral / follow-ups**:
  - **AIM and adm3 features**: not emitted (matches the SYCL ADM
    scope; AIM would need a separate kernel). No shipped model
    requires them.
  - The shared-memory `s_csf` / `s_cm` arrays in stage 3 are sized
    `[16]` (max 16 subgroups × 16-thread WG). For hardware with
    smaller subgroup sizes (e.g. wave64 on RDNA), the WG configuration
    may need re-sizing — fine for the current 256-thread WG with
    subgroup_size=32 default.
  - Motion3 (5-frame-window mode) remains deferred per ADR-0177.
  - **NVIDIA-Vulkan numerical drift** on `adm_scale2` (~2.4e-4 vs CPU,
    not reproduced on Arc). Likely host-side reduction order on the
    NVIDIA driver. Tracked as a kernel-side fix candidate (Kahan sum
    or replicate CPU loop order).
  - **NVIDIA-Vulkan dispatch overhead**: a 48-frame benchmark put
    Vulkan-on-RTX-4090 at ~840 fps vs CUDA-on-RTX at ~970 fps. With
    a 2400-frame fixture (overhead amortised) RTX hits ~11k fps on
    CUDA and ~820 fps on Vulkan — a 13× per-GPU gap that we attribute
    to NVIDIA's Vulkan submit/sync cost, not the kernel.
  - **SYCL/Arc fp64 emulation slowdown**: `--backend sycl` on Arc
    A380 logs `device lacks fp64 support — using int64 emulation
    for gain limiting`. Empirical 5–10× slowdown vs Vulkan/Arc on
    the same GPU. SYCL kernel needs an fp32 / int64 path for fp64-
    less devices.
  - **Vulkan + CUDA dispatcher conflict**: when both backends'
    state is imported, `vmaf_get_feature_extractor_by_feature_name`
    returns the first registry match (CUDA wins, Vulkan dead). New
    `--backend {auto,cpu,cuda,sycl,vulkan}` CLI flag in this PR
    forces exclusivity; auto behaviour preserved.

## References

- ADR-0177 — Vulkan motion kernel + cross-backend gate generalization.
- ADR-0176 — Vulkan VIF cross-backend gate (the gate this kernel
  joins).
- ADR-0175 — Vulkan backend scaffold.
- ADR-0127 — Vulkan backend governance decision.
- PR #119 — T5-1c motion (commit `32e31e45`).
- PR #118 — T5-1b-v cross-backend gate + CLI (commit `50758ea8`).
- `req` — user direction 2026-04-25 (paraphrased): "Two PRs: motion +
  motion_v2 first, ADM second". Selected via popup. PR #119 closed
  the first; this ADR's PR closes the second.
- `req` — user direction 2026-04-26 (paraphrased): "the all-zero
  diff is suspicious — did you actually run on Arc or was it CPU
  fallback?" That callout drove the bug investigation in the §"Bug
  history" section above. The four resulting fix commits in PR #120
  are `6167e300` (3 fixes), `de65c0ac` (header rename),
  `2aa79db1` (`--backend` flag + script extension), and the
  retroactive errata for ADR-0176 / ADR-0177.
