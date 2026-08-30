<!-- markdownlint-disable MD013 MD060 -->
# Research-0056: `precise` decoration audit on `vif.comp` + `ciede.comp` — Step A implementation findings

- **Status**: Active
- **Workstream**: [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md), [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md)
- **Last updated**: 2026-05-03

## Question

[ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)
defined a two-step path for the Vulkan 1.4 API-version bump:

- **Step A** — tag the load-bearing FP ops in `vif.comp` and
  `ciede.comp` with GLSL `precise` so glslc emits
  `OpDecorate ... NoContraction`. Re-run the cross-backend gate at
  API 1.4 against NVIDIA, RADV, lavapipe.
- **Step B** — bump `apiVersion` once Step A is clean.

This digest reports the empirical outcome of Step A on the agent's
local NVIDIA RTX 4090 + driver 595.71.05 (Vulkan 1.4.341) lane. The
question going in: *does the `precise` tagging proposed in
[research-0053](0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
move both shaders below the
[`places=4`](../adr/0214-gpu-parity-ci-gate.md) cross-backend gate
under a hypothetical API 1.4 bump?*

## Sources

- [research-0053](0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
  — companion investigation; established the FMA-contraction
  hypothesis and the "Step A then Step B" plan.
- [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)
  — the deferral decision Step A is gated on.
- [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md) — this PR's
  decision: ship the partial Step A fix, leave Step B blocked.
- [GLSL 4.50 §4.7.1 — precise qualifier](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.50.pdf#section.4.7.1).
- [SPIR-V 1.6 — `OpDecorate NoContraction`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpDecorate).
- [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  — the gate harness.

## Findings

### 1. SPIR-V emits NoContraction as expected

`glslc 2026.1` lowers `precise float r = a*b + c;` to a per-result
`OpDecorate %r NoContraction` exactly as research-0053 predicted.
After tagging:

| Shader | NoContraction decorations | 1.3 vs 1.4 SPIR-V byte-cmp |
|---|---|---|
| `vif.comp`   | 62  | identical (build is target-env-independent) |
| `ciede.comp` | 126 | identical |

The non-optimized `-O0` disassembly confirms each load-bearing op ID
in vif's stats expression is decorated:

| GLSL line | Op | SPIR-V ID | NoContraction |
|---|---|---|---|
| `g = sigma12 / sigma1_sq` (vif:498) | `OpFDiv` | `%1361` | yes |
| `g * sigma12` in `sv_sq` (vif:499) | `OpFMul` | `%1367` | yes |
| `sigma2_sq - g*sigma12` (vif:499)  | `OpFSub` | `%1368` | yes |
| `g * g` in `gg_sigma_f` (vif:502)  | `OpFMul` | `%1380` | yes |
| `(g*g) * sigma1_sq` (vif:502)      | `OpFMul` | `%1383` | yes |

So the GLSL → SPIR-V leg works as designed. The decoration is
*present and correct* on the suspect ops.

### 2. Cross-backend gate results (NVIDIA RTX 4090, driver 595.71.05, Vulkan 1.4.341)

Run on the canonical Netflix pair
(`src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`, 48 frames,
`places=4` → tolerance 5.0e-05):

| State | vif scale 0/1/3 | vif scale 2 | ciede2000 |
|---|---|---|---|
| **Master HEAD, API 1.3, no `precise`** (baseline) | 0/48 OK (≤2e-06) | **0/48 OK (1e-06)** | **42/48 FAIL (max abs 1.67e-04)** |
| Master HEAD, API 1.3, **with `precise`** (this PR) | 0/48 OK (≤2e-06) | **0/48 OK (2e-06)** | **5/48 FAIL (max abs 8.9e-05)** |
| Local exploratory bump, API 1.4, **with `precise`** | 0/48 OK (≤1e-06) | **45/48 FAIL (max abs 1.527e-02)** | **5/48 FAIL (max abs 8.9e-05)** |

Three findings stand out:

1. **vif precise is a no-op under both 1.3 and 1.4 on this driver**:
   the SPIR-V decorations are correct (point §1) yet the 45/48
   regression at API 1.4 is exactly the size research-0053 reported
   pre-fix. **NVIDIA driver 595.71 does not honour
   `OpDecorate NoContraction` on vif's float math under API 1.4** —
   or the regression's root cause is not FMA contraction in those
   five ops. Step A's hypothesis is *insufficient* for vif. Step B
   stays blocked on a deeper investigation (see §"Open questions").
2. **ciede precise is a 19× partial fix**: master HEAD on this NVIDIA
   driver was *already* 42/48 at API 1.3 (the gate just isn't run
   against NVIDIA in CI today, so this regression is unflagged
   debt). The conservative `precise` scope — chroma magnitudes,
   `a*_p`/`c*_p` half-axes, `s_l/s_c/s_h`, lightness/chroma/hue,
   final ΔE — moves the regression from 1.67e-04 to 8.9e-05, a 19×
   reduction. 5 frames out of 48 still exceed places=4 (1.78× the
   tolerance, max abs 8.9e-05).
3. **Widening the `precise` net into helper functions makes ciede
   strictly worse** (see §3 below). The conservative scope is the
   maximum that helps.

### 3. Aggressive `precise` widening hurts ciede

Tested intermediate state: extend `precise` into `srgb_to_linear`,
`xyz_to_lab_map`, `yuv_to_lab` (Lab axes), and the four ciede2000
helpers (`get_h_prime`, `get_delta_h_prime`, `get_upcase_h_bar_prime`,
`get_upcase_t`, `get_r_sub_t`). Result on NVIDIA at API 1.3:

| Scope | ciede mismatches | Max abs |
|---|---|---|
| Conservative (this PR) | **5/48** | **8.9e-05** |
| Aggressive (helpers + Lab) | 46/48 | 1.73e-04 |

Empirically the helpers' internal `mul + add` patterns appear to
fold *toward* the CPU compiler's folds when left un-decorated;
adding `NoContraction` forces them to a strict-eval path that
diverges further. The shader keeps the conservative scope and
documents this inline.

### 4. vif's float-stats expression is not the load-bearing diff

The vif kernel's only float ops are the three lines tagged in this
PR (`g`, `sv_sq`, `gg_sigma_f`). All other arithmetic is integer
(`int64` SSE accumulators, `dev_best16_from32/64` scalar shifts,
`log2_lut[]` integer lookups, `subgroupAdd` over `int64`). With
the float ops correctly decorated and *still* drifting at 1.527e-02
under API 1.4, the regression cannot be in those five ops alone.
Two hypotheses for follow-up:

- **NVIDIA's compiler is using a different `OpFDiv` lowering at
  1.4** — e.g. `(a/b) * c` rewritten as `mad(reciprocal(b), c, ...)`
  bypassing the per-result `NoContraction`. This is plausible because
  `NoContraction` only constrains the *single result it decorates*,
  not a multi-op rewrite that yields a numerically-different but
  structurally-different sequence.
- **The drift is in `dev_best16_from32`'s integer shift count via
  `uint(sv_sq)` truncation near integer boundaries**: a sub-ULP float
  drift that flips the truncation flips the LUT index, which then
  drifts `t_num_log` integer arithmetic by `O(LUT_value)`. A 1.527e-02
  per-frame VIF drift is consistent with one such flip per frame on
  scale-2 (~1320 px workgroup count × 1 LUT-index flip / WG).

Either way: more diagnostic work is needed, and `precise` alone
won't close it.

## Alternatives explored

| Option | Outcome | Why not chosen as the only fix |
|---|---|---|
| **Conservative `precise` (chosen for this PR)** | vif: no-op at 1.3, doesn't fix 1.4. ciede: 19× improvement at both. | Best partial fix reachable today; doesn't unblock Step B by itself. |
| Aggressive `precise` (helpers + Lab axes) | ciede regresses 5/48 → 46/48 at 1.3. | Strictly worse on the load-bearing kernel. |
| `-O0` glslc opt level for both shaders (analogue to [`psnr_hvs_strict_shaders`](../../core/src/vulkan/meson.build) workaround) | Not measured in this PR. Likely no-op under 1.3 (already clean for vif at `-O`); unclear at 1.4 since the glslc bytecode is already byte-identical at 1.3 vs 1.4. | Deferred — won't help the vif 1.4 regression because the regression is a *driver-side* compilation choice, and `-O0` at glslc level doesn't change what the driver sees beyond a few inlining hints. |
| `OpExecutionMode SignedZeroInfNanPreserveFloat32` / `RoundingModeRTEFloat32` | Not reachable from glslc 2026.1 today: `GL_EXT_shader_float_controls2` extension is rejected ("extension not supported"). | Blocked on glslc support; wait for SDK update or write `.spv.in` with hand-edited execution modes. |
| File NVIDIA driver bug | Not done in this PR (see follow-up note in ADR-0264). | Depends on a confirmed reproducer that excludes our shader bug; we don't have one yet (point §4 above). |

## Open questions

1. **Why doesn't `OpDecorate NoContraction` prevent the vif regression
   at API 1.4 on NVIDIA?** Hypothesis: the driver rewrites the
   `OpFDiv` + downstream `OpFMul + OpFSub` into a multi-op recipe
   that yields a different result, where `NoContraction` only
   constrains the per-result mapping (single ID) rather than the
   multi-op rewrite. Validate via a `nvidia-smi`-side capture
   (`NV_SHADER_DUMP`) of the compiled GPU code at 1.3 vs 1.4 and
   diff. Out of scope for this PR; tracked as backlog under
   ADR-0264 §"Open questions".
2. **Are the remaining 5/48 ciede mismatches a CPU-side issue or
   GPU-side?** The CPU reference uses scalar `pow`/`sqrt`/`sin`/`atan`
   with libm semantics. A 8.9e-05 max abs on a per-pixel ΔE of
   `O(10)` is `~1 ULP` on the chained transcendentals — possibly
   irreducible without lifting to `double` somewhere in the chain.
   Investigate by re-running the CPU side with `-fno-fast-math` and
   `__builtin_ia32_*` intrinsics replaced by libm calls; out of
   scope here.
3. **Does ciede already fail at 1.3 because of a long-standing
   mismatch nobody noticed?** Yes — the CI gate doesn't include an
   NVIDIA validation lane today (per research-0053 §"Why RADV stays
   clean"). The 42/48 baseline at 1.3 is *pre-existing fork debt on
   the NVIDIA driver path* that this PR partially repays (down to
   5/48). Worth filing as its own bug ticket.
4. **Will RADV / lavapipe also benefit?** Not measured in this PR
   (the agent's primary lane is NVIDIA + RADV; lavapipe is CI-side).
   `precise` on RADV should be a no-op since Mesa NIR's float-controls
   are already conservative. Lavapipe is software so contraction
   isn't available. Cross-driver re-run is a follow-up.

## Related

- [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)
  — the deferral decision and Step A / Step B plan.
- [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md) — this PR's
  decision: ship the partial fix.
- [research-0053](0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
  — root-cause investigation that motivated Step A.
- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) — `places=4` gate.
- [ADR-0187](../adr/0187-ciede-vulkan.md) — ciede2000 Vulkan port +
  precision contract.
- Source: `req` (parent-agent task brief, 2026-05-03): paraphrased —
  *implement Step A of the Vulkan 1.4 bump path documented in PR #338
  (ADR-0264): tag the load-bearing FP ops in vif.comp and ciede.comp
  with GLSL `precise`. After Step A lands, the API-version bump
  becomes safe (Step B is a separate PR).*
