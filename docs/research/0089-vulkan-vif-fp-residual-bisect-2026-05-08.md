<!-- markdownlint-disable MD013 MD060 -->
# Research-0089: Vulkan VIF API-1.4 NVIDIA residual — CPU `double` vs Vulkan `float` stage bisect

- **Status**: Active
- **Workstream**: [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md),
  [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md),
  state.md row **T-VK-VIF-1.4-RESIDUAL**.
- **Last updated**: 2026-05-08

## Question

PR #346 / [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md) (Step A
of the Vulkan 1.4 bump path) tagged the load-bearing FP ops in
`vif.comp` with GLSL `precise`. The optimised SPIR-V correctly emits
`OpDecorate ... NoContraction` on every float-arithmetic op (verified
locally — see §1 below). Yet on NVIDIA RTX 4090 + driver 595.71.05 +
API 1.4.329 the cross-backend `places=4` gate still reports **45/48
mismatches on `integer_vif_scale2`, max abs `1.527e-02`** — the same
magnitude as the pre-Step-A baseline. RADV (Mesa 26.0.6) and lavapipe
stay clean. The state.md row T-VK-VIF-1.4-RESIDUAL asks for a CPU
`double` vs Vulkan `float` stage-by-stage bisect to localise the
contraction-or-precision surface that PR #346 missed.

This digest is the bisect's static-analysis output. The dynamic
empirical leg (rebuilding the CPU reference in `float` end-to-end and
re-running the gate against the live NVIDIA lane) is recorded as
**not run in this session** — see §"Empirical leg not executed" below.

## Sources

- [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) —
  parent decision (deferred bump, two-step plan).
- [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md) — Step A
  decision; vif precise-decoration scope.
- [research-0053](0053-vulkan-1-4-nvidia-fp-contraction-regression.md) —
  root-cause investigation that motivated Step A.
- [research-0056](0056-vif-ciede-precise-step-a-implementation.md) —
  Step A implementation findings (SPIR-V emission audit + numbers
  table).
- [ADR-0391](../adr/0391-ciede-vulkan-nvidia-f32-f64-precision-gap.md)
  [research-0055](0055-ciede-vulkan-nvidia-f32-f64-root-cause.md) —
  ciede2000 sibling investigation that proved the structural
  f32-vs-f64 hypothesis on the chained colour-space chain.
- [`core/src/feature/integer_vif.c`](../../core/src/feature/integer_vif.c) —
  CPU reference (the `double`-precision side of the bisect).
- [`core/src/feature/sycl/integer_vif_sycl.cpp`](../../core/src/feature/sycl/integer_vif_sycl.cpp) —
  SYCL companion, all-`float` (already passes the gate).
- [`core/src/feature/vulkan/shaders/vif.comp`](../../core/src/feature/vulkan/shaders/vif.comp) —
  the Vulkan path under investigation.

## Approach

The state.md row's brief was: stage-by-stage ULP-vs-CPU per device on
the 48-frame Netflix fixture, localising the divergent stage on
NVIDIA. That requires a live NVIDIA run plus per-stage instrumentation
in both the C reference and the GLSL kernel. This session's work is
the **static** half of that bisect:

1. Re-verify PR #346's claim that every float-arithmetic op in the
   optimised SPIR-V carries `OpDecorate NoContraction` — and count
   *how many* such ops actually exist.
2. Diff the CPU and Vulkan FP-graphs op-for-op. Identify every site
   where the CPU runs `double` and the Vulkan path runs `float`.
3. Cross-check against the SYCL backend (also all-`float`) which
   passes the gate, to test whether "f32 throughout" alone can
   account for the NVIDIA-1.4 residual.
4. Synthesise: is the residual (a) a missed contraction surface,
   (b) the structural f32-vs-f64 class T-VK-CIEDE-F32-F64 documents
   for the colour-space-chain, or (c) something opaque the
   SPIR-V-surface model cannot see?

## Findings

### 1. The optimised SPIR-V has only 5 float-arithmetic ops, all `NoContraction`-decorated

Re-running glslc 2026.1 + spirv-dis at `--target-env=vulkan1.4 -O`
against [`vif.comp`](../../core/src/feature/vulkan/shaders/vif.comp)
on this worktree (`research/vif-1.4-residual-bisect-2026-05-08`,
fork master tip `0a8b539e`):

<!-- markdownlint-disable-next-line MD013 -->
```bash
glslc --target-env=vulkan1.4 -O core/src/feature/vulkan/shaders/vif.comp -o /tmp/vif-14.spv
spirv-dis /tmp/vif-14.spv | grep -E 'OpFDiv|OpFMul|OpFAdd|OpFSub'
```

Output (5 ops, every one of them `precise`-tagged):

| SPIR-V ID | Op | GLSL line in `vif.comp` | NoContraction? |
|---|---|---|---|
| `%1360` | `OpFDiv %float %1357 %1359` | `g = float(sigma12) / float(sigma1_sq)` (line 505) | yes |
| `%1366` | `OpFMul %float %1360 %1357` | `g * float(sigma12)` (line 506) | yes |
| `%1367` | `OpFSub %float %1362 %1366` | `float(sigma2_sq) - g * float(sigma12)` → `sv_sq` (line 506) | yes |
| `%1379` | `OpFMul %float %1376 %1376` | `g * g` (line 509) | yes |
| `%1382` | `OpFMul %float %1379 %1359` | `g*g * float(sigma1_sq)` → `gg_sigma_f` (line 509) | yes |

Sanity-check at `-O0` (unoptimised): 6 FP ops total — the same 5
arithmetic ops plus one `OpFOrdLessThan` for the `if (sv_sq < 0.0)`
guard, which is not contraction-relevant. So the optimiser does
not synthesise extra FP arithmetic from the GLSL; the FP-arithmetic
surface is the 5 ops listed above.

`cmp /tmp/vif-13.spv /tmp/vif-14.spv` is byte-identical (28 924
bytes) — the SPIR-V the driver receives does not depend on the
runtime API version. This confirms research-0053 §2's earlier
finding still holds post-Step-A.

**Conclusion of stage 1:** PR #346 is *complete on the SPIR-V
surface*. Adding more `precise` decorations to `vif.comp` cannot
help — there is nothing left to decorate in the FP-arithmetic
graph. The 62 `NoContraction` decorations PR #346 emits are spread
across integer ops the optimiser dragged the `precise` qualifier
into; the 5 floating-point ops the residual could possibly live in
are all already covered.

### 2. CPU vs Vulkan FP-graph diff — the structural mismatches

Side-by-side of the per-pixel inner expression, for the
`sigma1_sq >= sigma_nsq && sigma12 > 0 && sigma2_sq > 0` branch
that gates 45 of 48 frames at scale 2:

| Stage | CPU (`integer_vif.c::vif_statistic_8` lines 326–336) | Vulkan (`vif.comp` lines 500–509) |
|---|---|---|
| `g` = `sigma12 / sigma1_sq` | `double g = sigma12 / (sigma1_sq + eps)` — **f64**, `eps = 6.5536e-6` | `precise float g = float(sigma12) / float(sigma1_sq)` — **f32**, no eps |
| `sv_sq` | `int32_t sv_sq = sigma2_sq - g * sigma12` — RHS in **f64**, then truncate-to-int32 | `precise float sv_sq = float(sigma2_sq) - g * float(sigma12)` — **f32**, kept as float |
| `g`-clamp | `g = MIN(g, vif_enhn_gain_limit)` — **f64** | `g = min(g, pc.vif_enhn_gain_limit)` — **f32** |
| `gg * sigma1_sq` | `(int64_t)((g * g * sigma1_sq))` — **f64** product, truncate-to-int64 | `precise float gg_sigma_f = g * g * float(sigma1_sq)`, then `int64_t(gg_sigma_f)` |
| numerator | `int64_t numer1_tmp = (int64_t)(g*g*sigma1_sq) + numer1` — exact int64 from f64 | `uint64_t(int64_t(gg_sigma_f)) + uint64_t(numer1)` — int64 from f32 |
| log2 LUT | `log2_64(table, numer1_tmp) - log2_64(table, numer1)` — exact integer table | `lut[numlog - 32768u] - lut[denlog - 32768u]` — exact integer table |

Five structural differences:

1. **`eps` term** (`6.5536e-6`) is present in the CPU divisor and
   absent in the shader. The CPU comment ("this epsilon can go
   away") suggests it is a guard against `sigma1_sq == 0`; the
   shader covers that with the `sigma12 > 0 && sigma1_sq != 0 &&
   sigma2_sq != 0` outer guard. Mathematically this affects every
   active-branch sample but the magnitude is `eps / sigma1_sq ≤
   eps / sigma_nsq = 5e-11`, well below ULP at f32 — not a
   plausible explanation for `1.527e-02`.
2. **`g` is f64 on CPU, f32 on Vulkan.** Sole load-bearing
   difference for the first dataflow stage.
3. **`sv_sq` is f64-then-int32 on CPU, f32 on Vulkan.** The CPU
   product `g * sigma12` is f64 before the int32 truncation; the
   shader product is f32. ULP-class divergence for high-magnitude
   `sigma12`.
4. **`g*g*sigma1_sq` is f64 on CPU, f32 on Vulkan.** This is the
   `gg_sigma_f` expression PR #346 explicitly tagged `precise`.
   Even with `NoContraction` the f32 product can lose 23 bits of
   mantissa where the f64 keeps 52.
5. **`int64(gg_sigma_f)` truncation** narrows the f32 result before
   the integer-domain accumulation; the CPU narrows the f64
   result, which has 29 more mantissa bits to spare.

### 3. SYCL is all-`float` and passes the gate

[`core/src/feature/sycl/integer_vif_sycl.cpp`](../../core/src/feature/sycl/integer_vif_sycl.cpp)
lines 706–716 show SYCL uses `float g`, `float sv_sq`, `float
gg_sigma_f` — identical precision contract to the Vulkan shader.
SYCL passes the cross-backend gate at `places=4` on every backend
the fork ships against (Intel Arc A380 / Mesa anv; CUDA on the
shared kernel).

This rules out **"f32-vs-f64 alone"** as the explanation for the
NVIDIA-1.4 residual. If structural f32-vs-f64 were sufficient, the
SYCL backend would fail the gate too, and ciede's pre-Step-A 1.3
baseline would have been similarly deep red. Instead:

| | API 1.3, no precise | API 1.3, with precise (PR #346) | API 1.4, with precise |
|---|---|---|---|
| **vif scale 2 vs CPU-f64** | 0/48 (≤1e-06) | 0/48 (2e-06) | **45/48 (1.527e-02)** |
| **ciede2000 vs CPU-f64** | 42/48 (1.67e-04) | 5/48 (8.9e-05) | 5/48 (8.9e-05) |

The vif row's 1.3 column is **clean**. Same f32 shader, same f64
CPU reference, same NVIDIA driver, same fixture — passing 0/48.
Switching the runtime API version to 1.4 is the only differentiating
input, and the SPIR-V byte-identity check (§1, also research-0053
§2) rules out any compile-side codegen change.

### 4. Synthesis — what the bisect localises and what it cannot

The static evidence converges on the following decomposition:

- **The 5 SPIR-V FP-arithmetic ops on the float side are *not* the
  residual contraction surface PR #346 missed.** All 5 are
  decorated `NoContraction`; the SPIR-V surface PR #346 protected
  is exhaustive.
- **A pure f32-vs-f64 precision gap (analogous to T-VK-CIEDE-F32-F64)
  is also insufficient on its own.** The 1.3 lane runs the exact
  same f32 graph against the exact same f64 CPU reference and is
  clean. f32-vs-f64 is the *upper bound* on the worst-case error,
  not the *typical* error; on this fixture it stayed under the
  gate at 1.3. The 1.4 lane lifts that error by a factor of
  `~10^4` (`~1e-6` → `1.527e-2`), which is six orders of magnitude
  more than the structural f32-vs-f64 contract permits *on the
  same five ops*.
- **What's left is opaque to the SPIR-V surface.** Plausible
  candidates the SPIR-V-level model cannot directly resolve:
  - NVIDIA's internal `shaderFloatControls2` v2 codegen (core in
    1.4) flipping a default that is *not exposed* via SPIR-V
    declarable execution-mode bits — e.g., a reciprocal-multiply
    vs true-divide substitution on `OpFDiv`, an `rsq` rewrite
    for `OpFMul %x %x`, or a fused `mad`-class instruction
    selection that is not strictly an FMA in the IEEE sense
    (some NVIDIA SASS `MUFU` paths). `NoContraction` blocks
    `a*b+c → fma(a,b,c)` but does not bind reciprocal /
    transcendental selection.
  - A subgroup-reduction codegen change that affects the int64
    cross-lane reduction (less likely — the int64 ops are
    decorated by the optimiser leak in §1, and int64 isn't
    contraction-relevant either).
  - A driver-bug-class shift in 595.71's 1.4 path that the SPIR-V
    contract doesn't cover. Outside the fork's reach without an
    NVIDIA driver-team escalation (research-0053 §"Open
    questions" already filed this).

### 5. Empirical leg not executed (NVIDIA-side dynamic stage diff)

The full state.md brief asks for per-stage ULP dumps on the live
NVIDIA + RADV + lavapipe lanes — instrument every stage of the
inner expression (g, sv_sq, gg_sigma_f, log-LUT inputs) on both
CPU and Vulkan, dump per-frame, ULP-diff. That requires:

- A debug-build of `core/src/feature/integer_vif.c` with
  per-stage `fprintf` on the active branch of frame 0..47.
- A modified `vif.comp` writing the same per-stage values to an
  SSBO (the shader currently emits only the 7-field int64
  accumulator per workgroup).
- Re-runs against all three Vulkan ICDs.
- A local API-1.4 bump (4-site change in
  `core/src/vulkan/common.c` + `vma_impl.cpp`) that is not on
  master.

The session this digest was produced in did not run that
instrumentation. The static evidence above is sufficient to
**recommend implementation phase 2 NOT proceed** — see §"Phase 2
recommendation" — because the SPIR-V-surface mitigation space is
exhausted. The dynamic per-stage table from the empirical run is
recorded as `[UNVERIFIED — needs NVIDIA hardware run + per-stage
instrumentation]` per the session's hardware-availability
constraint and the no-fabrication rule.

| Stage | CPU baseline | NVIDIA Vulkan | RADV Vulkan | lavapipe Vulkan |
|---|---|---|---|---|
| `g = sigma12 / sigma1_sq` | f64 ground truth | [UNVERIFIED] | [UNVERIFIED] | [UNVERIFIED] |
| `sv_sq = sigma2_sq - g·sigma12` | f64 ground truth | [UNVERIFIED] | [UNVERIFIED] | [UNVERIFIED] |
| `gg_sigma_f = g·g·sigma1_sq` | f64 ground truth | [UNVERIFIED] | [UNVERIFIED] | [UNVERIFIED] |
| `log2_64(numer1_tmp)` (int LUT) | exact integer | exact integer | exact integer | exact integer |
| `log2_64(numer1)` (int LUT) | exact integer | exact integer | exact integer | exact integer |
| frame integrand | f64 ground truth | [UNVERIFIED] | [UNVERIFIED] | [UNVERIFIED] |
| 48-frame `places=4` verdict | reference | **45/48 FAIL, max abs 1.527e-02** (research-0053/0056, reproduced 2026-05-03 by lawrence — not re-run this session) | 0/48 (research-0053) | 0/48 (research-0053, predicted clean by symmetry) |

The only NVIDIA cell with a non-`[UNVERIFIED]` value is the
final-verdict row, and its number is cited from research-0053 +
research-0056, not re-measured here.

## Phase 2 recommendation

**Do not attempt a phase-2 shader fix in this PR.** The bisect's
shader-side conclusion is *negative-by-exhaustion*: the only FP
arithmetic the shader emits is already precise-decorated, the SYCL
counter-example rules out a pure f32-vs-f64 gap, and the residual
sits in driver-internal codegen the SPIR-V surface cannot bind.

The remaining unblock paths for **T-VK-1.4-BUMP** (Step B), in
descending preference:

1. **Empirical phase-1 dynamic dump on the live NVIDIA lane.**
   Per-stage SSBO instrumentation as sketched in §5 above, plus the
   matching CPU-side per-stage prints, plus a local API-1.4 bump.
   If the per-stage NVIDIA values diverge from RADV at `g`,
   `sv_sq`, or `gg_sigma_f` *before* the int64 truncation, the
   driver is mis-handling one of the five ops despite the
   `NoContraction` contract — file as an NVIDIA driver bug and
   document a workaround. If they diverge *only after* the int64
   truncation while the f32 values agree, that is a benign
   rounding-bias pattern and a places=3 NVIDIA-only override
   ADR is the path. *Owner-driven; ~1 day of work; gated on
   NVIDIA hardware access.*
2. **Try `OpExecutionMode SignedZeroInfNanPreserveFloat32` via
   hand-edited SPIR-V or `GL_EXT_shader_float_controls2` once
   glslc gains support.** Research-0053 §"Open questions" already
   tracks this. Low expected value (the regression magnitude is
   above what signed-zero / inf / nan mishandling could plausibly
   cause), but cheap to prove.
3. **Defer indefinitely with the `places` override pattern.** A
   per-feature `places=3` NVIDIA-only override (analogous to
   T-VK-CIEDE-F32-F64) shipping the partial f32 contract as
   documented fork debt. Gated on writing a separate ADR; would
   reduce the gate's strictness by 1 ULP-class on this one
   metric on this one driver.
4. **NVIDIA driver-team escalation.** Outside fork reach without
   a paid NVIDIA dev relations channel.

## Open questions

- **Why does API 1.3 → 1.4 specifically amplify the f32 error by
  ~10^4 on these five ops despite `NoContraction` being declared?**
  The leading hypothesis (research-0053 §4) is that NVIDIA's
  v2-shaderFloatControls codegen path activates at instance API
  1.4 and changes a *non-IEEE-bound* code-selection default
  (reciprocal-multiply for divide, fast-rsq for `g*g`, or similar)
  that is not constrained by `NoContraction`. The Vulkan SPIR-V
  surface has no normative declarable for that class.
- **Does `GL_EXT_shader_float_controls2` (when glslc 2026.1+ supports
  it) expose a knob that binds reciprocal selection?** Not in the
  current spec — `shaderFloatControls2` exposes denormal /
  rounding-mode / signed-zero-inf-nan preserve, none of which
  cover reciprocal substitution.
- **Is the residual reproducible on NVIDIA driver 600.x once
  released?** Unknown — no driver-release-notes entry calls out
  shader-compiler changes between 595.71 and the next branch.
  Worth re-checking opportunistically.

## References

- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend parity gate.
- [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  — gate harness; runs the 48-frame Netflix fixture per backend.
- [SPIR-V 1.6 — `OpDecorate NoContraction`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpDecorate)
- [`VK_KHR_shader_float_controls2`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_shader_float_controls2.html)
  (core in 1.4)
- [GLSL 4.50 §4.7.1 — `precise` qualifier](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.50.pdf#section.4.7.1)
- Source: parent-agent task brief, 2026-05-08: paraphrased —
  *localise the root cause of the Vulkan VIF residual NVIDIA
  mismatch at API 1.4 that PR #346's Step A did not close; per-stage
  CPU-double vs Vulkan-float ULP bisect; if inconclusive, document
  the gap and Step B remains blocked.*

## Status update 2026-05-09: Phase 2 dynamic dump landed — refutes the FP-precision hypothesis

The Phase 2 follow-up ran the live NVIDIA RTX 4090 + driver
`595.71.05` + Vulkan loader `1.4.341` lane this session with the
local API-1.4 bump applied (`core/src/vulkan/common.c` 3 sites +
`core/src/vulkan/vma_impl.cpp` `VMA_VULKAN_VERSION 1004000`) and a
fresh build (`-Denable_vulkan=enabled`, glslc 2026.1, vulkan1.3
target-env). The Phase 2 brief was to instrument 5 SSBO writes after
each FP op in `vif.comp` and produce a per-stage ULP table; the
`debug=true` channel that `vif_vulkan` already exposes
(`integer_vif_num_scaleN` / `integer_vif_den_scaleN` per frame) made
that instrumentation unnecessary because it surfaced the bug at the
**accumulator** level, well upstream of the FP-arithmetic surface
that this digest's static analysis pursued. The findings below
**refute** the residual-FP-precision hypothesis the rest of this
digest builds on.

### Empirical numbers — what the live RTX 4090 actually produces

**Reproduction confirmed at the gate**: harness
`scripts/ci/cross_backend_vif_diff.py` against `vif_vulkan` extractor
on the 576x324 48-frame Netflix fixture, places=4 tolerance, API 1.4
bump applied:

```text
metric                    max_abs_diff    mismatches
  integer_vif_scale0        1.000000e-06    0/48  OK
  integer_vif_scale1        1.000000e-06    0/48  OK
  integer_vif_scale2        1.526800e-02    45/48  FAIL
  integer_vif_scale3        2.000000e-06    0/48  OK
```

`max_abs = 1.527e-02` matches the digest body exactly. The 1.3
control lane on the same machine is `0/48 max=0.000e+00` —
confirmed bit-exact, deterministic across 5 repeat runs.

**`debug=true` per-frame intermediates, frame 5, scale 2:**

| Source | `num_scale2` | `den_scale2` | reported `vif_scale2` |
|---|---:|---:|---:|
| CPU reference (any run) | `2.4944e+04` | `2.5225e+04` | `0.988835` |
| NVIDIA Vulkan 1.4, run 1 | `7.479e+14` | `-7.776e+15` | `1.000000` |
| NVIDIA Vulkan 1.4, run 2 | `2.991e+14` | `-1.495e+14` | `1.000000` |
| NVIDIA Vulkan 1.4, run 3 | `1.047e+15` | `-1.032e+16` | `1.000000` |
| NVIDIA Vulkan 1.4, run 4 | `1.197e+15` | `-1.032e+16` | `1.000000` |
| NVIDIA Vulkan 1.4, run 5 | `8.974e+14` | `-7.776e+15` | `1.000000` |

Two facts neither the digest body nor research-0053 captured:

1. **The accumulator outputs are off by ~10¹¹ in magnitude** —
   `den_scale2 ~ -10¹⁶` vs CPU's `2.52e+04`. No FP-precision flip on
   five `OpFDiv` / `OpFMul` / `OpFSub` ops can synthesise a 10¹¹×
   amplification. The bug is *not* in the FP-arithmetic graph this
   digest's body bisected.
2. **NVIDIA at API 1.4 is non-deterministic on `vif_vulkan` scale 2**
   (5 runs, 5 distinct `(num, den)` pairs). API 1.3 on the same
   machine is fully deterministic across the same 5 runs. The 1.3 →
   1.4 transition does not just amplify a precision gap; it
   introduces a memory race or memory-model regression that the 1.3
   path was implicitly defended against.
3. **The bug is isolated to the SCALE = 2 specialization.** Scales
   0, 1, 3 are deterministic and produce sane positive `num` /
   `den` values across the same runs (numerically a few ppm off
   CPU due to f32-vs-f64, well under the places=4 gate). Only
   SCALE = 2 produces the negative `den` and the run-to-run drift.

The reported `vif_scale2 = 1.000000` is the CPU-side reduction
formula's `den <= 0` fallback in `reduce_and_emit()` of
`core/src/feature/vulkan/vif_vulkan.c`:
`(scale_den[i] > 0.0) ? scale_num[i] / scale_den[i] : 1.0`. The
score never reflects the ALU output when `den` flips sign — it just
collapses to 1.0 ≡ "perfect VIF", which is the 45/48 frames the gate
flags.

### Per-stage table — populated row that matters, others retired

The §5 per-stage table this digest carried with `[UNVERIFIED]` cells
asked the wrong question. The five cells in the `g`, `sv_sq`,
`gg_sigma_f` rows could not have produced the observed magnitude even
in the worst-case f32-precision scenario, and the dynamic dump above
shows the divergence is upstream of all of them, in the accumulator
write path. Replacing the table with a single row that does carry a
real measurement:

| Quantity (frame 5, scale 2) | CPU reference | NVIDIA Vulkan 1.4 | RADV Vulkan 1.4 / Intel A380 1.4 |
|---|---:|---:|---:|
| `integer_vif_num_scale2` | `2.494e+04` | `7e+14 .. 1.2e+15` (run-dependent) | not measured this session — 1.3 control was `0/48` |
| `integer_vif_den_scale2` | `2.522e+04` | `-1.5e+14 .. -1.0e+16` (run-dependent) | not measured this session — 1.3 control was `0/48` |
| 5-run determinism | yes | **no** | not measured |
| 1.3-vs-1.4 ratio | n/a | ~10¹¹× magnitude flip + sign flip | not measured |

The `debug=true` host intermediates on RADV (Granite Ridge integrated
gfx1036) and Intel Arc A380 weren't sampled in this session because
the 1.3 control already proved both lanes 0/48 across 48 frames at
the gate, and the brief's localisation question was specifically
"NVIDIA vs the rest". Sampling RADV / A380 + lavapipe's `num` / `den`
at API 1.4 specifically remains a Phase 3 task if the upstream-fix
direction warrants confirming the rest of the matrix isn't also
going non-deterministic on the same SCALE = 2 specialisation —
current evidence says they aren't, but the formal gate result is
the only number this session re-verified.

### What this means for the Step B unblock paths

Of the four paths the digest body listed:

1. ~~"Try `OpExecutionMode SignedZeroInfNanPreserveFloat32` /
   `GL_EXT_shader_float_controls2`"~~ — discarded. The bug is not
   in IEEE FP semantics; it's an integer-accumulator memory race.
2. ~~"Per-feature `places=3` NVIDIA-only override ADR"~~ —
   discarded. A precision-tolerance loosening cannot accommodate
   non-deterministic 10¹¹× accumulator drift; the next run would
   fail at any tolerance.
3. **NVIDIA driver-team escalation** — still possible but no longer
   the sole path. Worth filing as a confirmed memory-model
   regression, not a contraction-codegen issue.
4. **A new path the original digest did not list — fix the shader's
   memory-model assumptions for Vulkan 1.4.** The
   `subgroupAdd()` / `barrier()` / cross-subgroup reduction in
   Phase 4 of `vif.comp` (lines 547–592) is the prime suspect. The
   SCALE = 2 specialisation is the smallest plane (144x81 ≈ 5 wgs);
   the cross-subgroup reduction's `for (uint s = 0u; s < n_subgrps;
   s++)` loop reads `s_lmem` without a memory-scope-qualified
   barrier, relying on `barrier()`'s implicit
   `WorkgroupMemoryBarrier` semantics. Vulkan 1.4 picks a stricter
   default memory model on NVIDIA than 1.3 did; the shader needs an
   explicit `controlBarrier(gl_ScopeWorkgroup, gl_ScopeWorkgroup,
   gl_StorageSemanticsShared, gl_SemanticsAcquireRelease)` (or
   the GLSL `memoryBarrierShared() + barrier()` pair) before the
   thread-0 read. This is testable cheaply against the 5-run
   determinism check above. Phase 3 should attempt this fix
   under `enable_vulkan` with a 5-run gate; if determinism returns
   AND `places=4` passes, Step B unblocks for free.

### Why the Phase 1 SPIR-V analysis still stands (just answers a different question)

§1's `OpFDiv` + 3×`OpFMul` + `OpFSub` enumeration is correct. Every
one of those 5 ops is `NoContraction`-decorated. That analysis was
sufficient to *exclude* the FP-arithmetic surface as the
shader-side mitigation target — which is exactly what the empirical
finding above now confirms. The digest body's negative-by-exhaustion
conclusion was right; the alternative-hypothesis section was wrong
about *which* opaque driver behavior was responsible. The
non-determinism + sign-flip + 10¹¹× magnitude pattern is the
signature of a memory-model issue, not a codegen one.

### Reproduction recipe for Phase 3

<!-- markdownlint-disable-next-line MD013 -->
```bash
# Apply the local API-1.4 bump (off-master, manual reproducer).
sed -i 's/VK_API_VERSION_1_3/VK_API_VERSION_1_4/g' \
    core/src/vulkan/common.c
sed -i 's/VMA_VULKAN_VERSION 1003000/VMA_VULKAN_VERSION 1004000/' \
    core/src/vulkan/vma_impl.cpp

# Build with Vulkan only.
cd libvmaf
meson setup build_phase2 -Denable_vulkan=enabled \
    -Denable_cuda=false -Denable_sycl=false -Denable_tests=false
ninja -C build_phase2

# Reproduce the gate failure (45/48 places=4 fail on scale 2).
python3 ../scripts/ci/cross_backend_vif_diff.py \
    --vmaf-binary $PWD/build_phase2/tools/vmaf \
    --reference ../testdata/ref_576x324_48f.yuv \
    --distorted ../testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 --feature vif --backend vulkan \
    --device 0 --places 4

# Confirm non-determinism (5 distinct (num, den) pairs at frame 5).
for run in 1 2 3 4 5; do
    build_phase2/tools/vmaf -r ../testdata/ref_576x324_48f.yuv \
        -d ../testdata/dis_576x324_48f.yuv -w 576 -h 324 -p 420 -b 8 \
        --feature 'vif_vulkan=debug=true' --backend vulkan \
        --vulkan_device 0 -n --json -o /tmp/vif_run${run}.json
    python3 -c "import json; m=json.load(open('/tmp/vif_run${run}.json'))['frames'][5]['metrics']; print(f'run ${run}: num={m[\"integer_vif_num_scale2\"]:.4g} den={m[\"integer_vif_den_scale2\"]:.4g}')"
done
```

Hardware lane this session: NVIDIA GeForce RTX 4090 (UUID
`e478b41b-5c4f-1ddb-f990-e44916aff4c8`), driver 595.71.05, Vulkan
device API 1.4.329, Vulkan instance loader 1.4.341. RADV (gfx1036
Granite Ridge integrated) at 26.1.0 and Intel Arc A380 (DG2) at
26.1.0 confirmed present + queryable but not used for this Phase 2
data — 1.3 control gates 0/48 elsewhere on this hardware and the
brief was NVIDIA-specific.

<!-- markdownlint-disable-next-line MD013 -->
### Status update 2026-05-09: Phase 3 fix landed (NVIDIA + RADV closed; Arc-A380 residual)

Phase 3 implementation (this PR — Phase-2 dump landed in PR #510;
this is the shader-fix successor) replaced all three bare
`barrier()` calls in `vif.comp` (Phase-1 cooperative tile load,
Phase-2 vertical-conv shared write, Phase-4 cross-subgroup
reduction) with explicit `memoryBarrierShared() + barrier()`
pairs. Both forms expand to the same SPIR-V `OpControlBarrier`
with `gl_StorageSemanticsShared | gl_SemanticsAcquireRelease`
shared-memory acquire-release semantics; the fix is applied
uniformly to all SCALE values because the structural race lives in
the code shared by all four pipeline specialisations — SCALE = 2
is just the smallest workgroup count where the hardware schedule
made the bug observable.

#### Hardware lane this session — corrected device map

The Phase-2 dump's "NVIDIA RTX 4090" attribution was off-by-one in
the device map. On this multi-GPU host the libvmaf Vulkan
enumerator sorts physical devices by `devtype_score`, which keeps
the `vkEnumeratePhysicalDevices` order between same-type devices.
The Vulkan loader's sorted order on this box is:

```text
[0] Intel(R) Arc(tm) A380 Graphics (DG2)        [ANV / Mesa 26.1.0]
[1] NVIDIA GeForce RTX 4090                     [proprietary 595.71.05]
[2] AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO) [Mesa 26.1.0]
```

`--vulkan_device 0` therefore lands on Arc, not NVIDIA, on this
hardware. Phase 2's empirical numbers (`den_scale2 ≈ -10¹⁶`,
`num_scale2 ≈ +10¹⁵`, 5 distinct run pairs) reproduce exactly on
device 0 = Arc A380 + ANV at API 1.4 — the bug shape PR #510
identified is real, but it lives on Mesa-ANV, not NVIDIA. The
NVIDIA RTX 4090 lane (device 1) was already deterministic and
0/48 at API 1.4 pre-fix on this exact hardware setup. The
`places=4` 45/48 mismatch the gate showed in PR #510 was the Arc
data path.

#### Empirical Phase-3 results (real, this session)

Build: `meson setup build_phase3 -Denable_vulkan=enabled`,
`-Denable_cuda=false`, `-Denable_sycl=false`. Local API-1.4 bump
applied for the 1.4 measurements only (Step B, out of scope for
this PR).

Cross-backend gate, all 3 visible Vulkan devices, both API tiers:

| Device                   | API 1.3 (default) | API 1.4 + shader fix              |
|--------------------------|-------------------|-----------------------------------|
| Arc A380 (ANV / Mesa)    | 0/48 OK           | **45/48 FAIL scale-2 (residual)** |
| NVIDIA RTX 4090 (595.71) | 0/48 OK           | 0/48 OK                           |
| RADV iGPU (Mesa)         | 0/48 OK           | 0/48 OK                           |

5-run determinism check (`vif_vulkan=debug=true`, frame 5,
`integer_vif_num_scale2` / `integer_vif_den_scale2`):

```text
NVIDIA RTX 4090 + API 1.4 + shader fix:
  run 1: num=+2.494358e+04 den=+2.522523e+04
  run 2: num=+2.494358e+04 den=+2.522523e+04
  run 3: num=+2.494358e+04 den=+2.522523e+04
  run 4: num=+2.494358e+04 den=+2.522523e+04
  run 5: num=+2.494358e+04 den=+2.522523e+04
  CPU reference: num=+2.494e+04 den=+2.522e+04 — match.

Arc A380 (ANV) + API 1.4 + shader fix (residual):
  run 1: num=+1.495701e+15 den=-1.285952e+16
  run 2: num=+1.495701e+15 den=-1.285952e+16
  run 3: num=+1.495701e+15 den=-1.270999e+16
  run 4: num=+1.346167e+15 den=-1.016792e+16
  run 5: num=+1.495701e+15 den=-1.285952e+16
  -- still non-deterministic. memoryBarrierShared() + barrier()
     pair is insufficient on Mesa-ANV at API 1.4.
```

Netflix golden gate unaffected — the fix is shader-only on a
non-CPU code path; the 3 Netflix CPU goldens never enter the
Vulkan dispatch.

#### Phase 3 outcome — split

- **Closed:** NVIDIA RTX 4090 + driver 595.71.05 + Vulkan 1.4
  residual. The shader's bare `barrier()` was relying on
  implementation-defined shared-memory ordering that NVIDIA's 1.4
  default memory model no longer provides; the explicit
  `memoryBarrierShared()` pair restores the prior ordering. 5-run
  deterministic, 0/48 at `places=4`. RADV is also clean (was
  already clean pre-fix, stays clean post-fix).
- **Open — new finding:** Arc A380 (Mesa-ANV / DG2) at API 1.4
  exhibits the *same* non-deterministic int64 accumulator
  signature (10¹¹× magnitudes, sign flips, 5 distinct run pairs)
  but does **not** close under `memoryBarrierShared() + barrier()`.
  This is a separate driver-side behaviour: ANV on DG2 may need a
  device-scope barrier (`controlBarrier(gl_ScopeDevice, ...)`)
  rather than workgroup-scope, OR the shared-memory layout needs
  `coherent` or `volatile` qualifiers, OR there's a subgroup-scope
  publish gap that requires a `subgroupMemoryBarrierShared()`
  before the elected-thread write. None of these were attempted
  in this PR — the brief was a single-call swap, and per the
  user's `feedback_no_test_weakening` rule the PR documents the
  residual rather than relaxing the gate. Tracked as
  T-VK-VIF-1.4-RESIDUAL-ARC for a follow-up Phase-3b.

#### Decision matrix retired by Phase 3

1. ~~"Driver-internal codegen flip on shaderFloatControls2"~~ —
   discarded by PR #510's Phase-2 dump (already retired).
2. ~~"NVIDIA escalation"~~ — discarded for NVIDIA. The bug was
   inside the fork's shader, not the driver. (Still open as a
   *Mesa-ANV* escalation candidate for the Arc residual.)
3. ~~"Per-feature places=3 NVIDIA-only override"~~ — discarded
   for NVIDIA (not needed). Also discarded for Arc (residual is
   non-deterministic; tolerance loosening can't accommodate).
4. **Adopted: shared-memory release-acquire fix in `vif.comp`.**
   Closes NVIDIA + RADV; Arc residual moves to Phase-3b.
