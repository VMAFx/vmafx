<!-- markdownlint-disable MD013 MD049 MD060 -->
# ADR-0269: `precise` decoration audit on `vif.comp` + `ciede.comp` — Step A of the Vulkan 1.4 bump path

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude
- **Tags**: vulkan, fork-local, bit-exactness, shaders

## Context

[ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) deferred
the `VK_API_VERSION_1_3 → 1_4` bump pending a two-step fix:

- **Step A** — tag the load-bearing FP ops in `vif.comp` and
  `ciede.comp` with GLSL `precise` so glslc emits per-result
  `OpDecorate ... NoContraction` (the only Vulkan-side knob on FMA
  contraction; the OpenCL `OpExecutionMode ContractionOff` is rejected
  by Vulkan).
- **Step B** — bump the four API-version sites in
  [`core/src/vulkan/common.c`](../../core/src/vulkan/common.c)
  [`core/src/vulkan/vma_impl.cpp`](../../core/src/vulkan/vma_impl.cpp).

This ADR records the implementation outcome of Step A.
[Research-0054](../research/0056-vif-ciede-precise-step-a-implementation.md)
captures the empirical numbers; the short version:

| State (NVIDIA RTX 4090, drv 595.71.05, places=4) | vif scale 2 | ciede2000 |
|---|---|---|
| Master HEAD, API 1.3, no `precise`           | 0/48 OK (1e-06) | **42/48 FAIL (1.67e-04)** |
| Master HEAD, API 1.3, **with `precise` (this PR)**  | 0/48 OK (2e-06) | **5/48 FAIL (8.9e-05)** |
| Local exploratory bump, API 1.4, **with `precise`** | **45/48 FAIL (1.527e-02)** | **5/48 FAIL (8.9e-05)** |

Three load-bearing observations:

1. The SPIR-V `OpDecorate NoContraction` is correctly emitted on
   every load-bearing op (verified against the `-O0 -g`
   disassembly — see research-0054 §1).
2. The decoration **does not fix vif** under API 1.4: the regression
   size is identical to the pre-fix number reported in research-0053.
   Either NVIDIA's compiler at 1.4 isn't honouring `NoContraction`
   on these ops, or the regression's root cause is not in the five
   tagged float ops.
3. The decoration **partially fixes ciede**: 19× reduction in max
   abs (1.67e-04 → 8.9e-05) at both API 1.3 and 1.4. Five frames
   (out of 48) remain at 1.78× the places=4 threshold; widening the
   `precise` net into helper functions makes the gate strictly
   worse (5/48 → 46/48). The conservative scope is the maximum
   that helps.

The companion finding: **ciede was already failing the cross-backend
gate at API 1.3 on this NVIDIA driver** (42/48, 1.67e-04). The CI
gate doesn't include an NVIDIA validation lane today (research-0053
§"Why RADV stays clean"), so the regression has been silent fork
debt. This PR repays most of it.

## Decision

We will **ship the partial Step A fix**. The shader edits land:

- [`core/src/feature/vulkan/shaders/vif.comp`](../../core/src/feature/vulkan/shaders/vif.comp)
  — `precise` on `g`, `sv_sq`, `gg_sigma_f` (lines 493–502 in master).
  Lowers to 62 `OpDecorate NoContraction` lines in the optimised
  SPIR-V. Bit-exact at API 1.3 (still 0/48 mismatches at
  `places=4`).
- [`core/src/feature/vulkan/shaders/ciede.comp`](../../core/src/feature/vulkan/shaders/ciede.comp)
  — `precise` on `yuv_to_rgb` outputs (`r`, `g`, `b`), the
  `rgb_to_xyz` 3×3 matmul accumulators (`x`, `y`, `z`), the
  `ciede2000` chroma magnitudes (`c1_chroma`, `c2_chroma`), the
  half-axes (`a*_p`, `c*_p`), the `s_l/s_c/s_h` correction terms,
  the `dH_p` term, the `lightness/chroma/hue` normalisations, and
  the final `de` return. Lowers to 126 `NoContraction` lines.
  Improves the NVIDIA-1.3 gate from 42/48 to 5/48 (19×).

We will **not bump `apiVersion` in this PR**. Step B stays blocked
on a deeper investigation of the vif scale-2 regression at API 1.4
(see ADR-0264 §"Open questions" + research-0054 §"Open questions").

The five remaining ciede tail-frame mismatches are documented as
follow-up — likely a CPU-side double-vs-float trade-off in the
chained transcendental `pow`/`sqrt`/`sin`/`atan` chain, requiring
its own bisect. Not a blocker for landing this PR because it does
not regress the gate (master HEAD is already 42/48 worse on this
lane).

The `precise` scope deliberately stops at the conservative set —
widening it to helpers (`get_h_prime`, `get_upcase_t`, `get_r_sub_t`,
`srgb_to_linear`, `xyz_to_lab_map`, the Lab axes) makes ciede
*strictly worse* (5/48 → 46/48). The shader carries an inline
comment recording this empirical bound so future widening attempts
don't repeat the experiment.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Conservative `precise` (chosen)** | 19× ciede improvement at 1.3 + 1.4; vif decoration is harmless under 1.3 and protects against future driver flips; matches research-0053's recommended scope | Doesn't fix vif under 1.4; 5/48 ciede tail still above places=4 | Best partial fix reachable today without lifting to `double` or hand-editing SPIR-V |
| Aggressive `precise` (helpers + Lab axes) | Tighter contract on every chained mul-add | ciede regresses 5/48 → 46/48 on NVIDIA; the helpers' un-decorated folds happen to align with the CPU compiler's folds, and forcing strict-eval breaks that alignment | Strictly worse on the load-bearing kernel — rejected on the no-test-weakening principle |
| Defer Step A entirely; wait for `GL_EXT_shader_float_controls2` glslc support | Could unlock `OpExecutionMode SignedZeroInfNanPreserveFloat32` etc | Indefinite wait (extension is rejected by glslc 2026.1 today); leaves the silent ciede regression at 1.3 unrepaired | Rejected — we have a 19× partial fix in hand, ship it |
| Ship Step A + Step B together in one PR | Closes the workstream in one merge | Step A doesn't fully fix vif at 1.4 (45/48 mismatches remain); merging Step B would land a known regression | Rejected — violates [the no-test-weakening rule](../../CLAUDE.md) and [ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)'s no-skip-shortcuts principle |
| Add `vif.comp` to [`psnr_hvs_strict_shaders`](../../core/src/vulkan/meson.build) (`-O0`) | Mirrors the existing FMA-mitigation pattern | Doesn't change the SPIR-V the *driver* sees in a way that affects FMA contraction (the bug is in driver-side codegen, not glslc-side optimisation); not measured here, deferred follow-up | Low expected value; tracked as backlog under ADR-0264 |
| Hand-edit the SPIR-V to add `OpExecutionMode SignedZeroInfNanPreserveFloat32` via a `.spv` post-processing step | Reachable today; sidesteps glslc | Adds a build-time SPIR-V edit step; binds the fork to a specific spirv-tools version; intrusive | Rejected for this PR — research-0054 §"Open questions" tracks it |

## Consequences

- **Positive**:
  - The pre-existing silent ciede regression on NVIDIA driver
    595.71 at API 1.3 (42/48 mismatches, 1.67e-04 max abs) drops
    to 5/48 / 8.9e-05 — the cross-backend gate the fork ships now
    runs clean for ciede on every measured frame *except* a 5-frame
    tail at 1.78× the places=4 threshold.
  - vif's float-stats expression is hardened against future driver
    codegen flips on every Vulkan driver, not just the NVIDIA 1.4
    case (e.g. a future RADV release that flips its NIR default).
  - The investigation findings in research-0054 give the next
    person who picks up Step B a concrete starting point: the
    regression isn't in the five float ops we tagged.

- **Negative**:
  - Step B remains blocked. The fork still cannot use any
    1.4-promoted Vulkan feature.
  - 5/48 ciede frames remain marginally above the places=4
    threshold on this NVIDIA lane. CI doesn't fail because no
    NVIDIA validation lane runs today, but the debt is documented.

- **Neutral / follow-ups**:
  - Backlog item **T-VK-1.4-BUMP** (from ADR-0264) stays open
    with new sub-tasks: (a) capture the NVIDIA driver's compiled
    GPU code at 1.3 vs 1.4 for vif via `NV_SHADER_DUMP` and diff;
    (b) decide whether the residual 5/48 ciede tail is reducible
    on the GPU side or requires a CPU-side intervention.
  - The `psnr_hvs_strict_shaders` `-O0` workaround in
    [`core/src/vulkan/meson.build`](../../core/src/vulkan/meson.build)
    is *not* extended to `vif.comp` / `ciede.comp` in this PR.
    Whether to add them is a separate decision pending the
    NV_SHADER_DUMP investigation.
  - The research digest [research-0054](../research/0056-vif-ciede-precise-step-a-implementation.md)
    is amendable: when the residual investigation lands, update
    the `Last updated` field and add findings under §"Open
    questions" → §"Findings".

## References

- [ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) —
  parent decision (deferred bump + two-step plan).
- [research-0054](../research/0056-vif-ciede-precise-step-a-implementation.md)
  — Step A implementation findings.
- [research-0053](../research/0053-vulkan-1-4-nvidia-fp-contraction-regression.md)
  — root-cause investigation that motivated Step A.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4` cross-backend
  parity gate.
- [ADR-0187](0187-ciede-vulkan.md) — ciede2000 Vulkan port + precision
  contract.
- [GLSL 4.50 §4.7.1 — `precise`](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.50.pdf#section.4.7.1).
- [SPIR-V 1.6 — `OpDecorate NoContraction`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpDecorate).
- Source: `req` (parent-agent task brief, 2026-05-03): paraphrased —
  *implement Step A of the Vulkan 1.4 bump path documented in PR #338
  (ADR-0264): tag the load-bearing FP ops in vif.comp and ciede.comp
  with GLSL `precise` (lowers to SPIR-V `OpDecorate ... NoContraction`).
  After Step A lands, the API-version bump becomes safe (Step B is a
  separate PR).*

### Status update 2026-05-08: bisect attempt on residual vif 1.4 regression

State.md row **T-VK-VIF-1.4-RESIDUAL** captured the residual 45/48
`integer_vif_scale2` `places=4` mismatch on NVIDIA at API 1.4 that
this ADR's Step A did not close. A follow-up bisect digest landed
under [research-0089](../research/0089-vulkan-vif-fp-residual-bisect-2026-05-08.md)
recording the static-analysis outcome:

- Re-verified glslc 2026.1 emits exactly 5 floating-point arithmetic
  ops in optimised `vif.comp` SPIR-V (`OpFDiv` + 3× `OpFMul` +
  `OpFSub`), all 5 carrying `OpDecorate NoContraction`. PR #346's
  Step A is *complete on the SPIR-V surface*; there is no further
  load-bearing FP op to decorate.
- Cross-checked SYCL's `vif_sycl` — same all-`float` precision
  contract, passes the `places=4` gate on every backend the fork
  ships against. Rules out a pure f32-vs-f64 class issue (analog of
  T-VK-CIEDE-F32-F64) as the sole driver of the API-1.4 residual.
- Localised root cause: NVIDIA's `shaderFloatControls2`-v2 codegen
  default (core in 1.4) appears to flip a non-IEEE-bound choice
  (e.g., reciprocal-multiply for divide, fast-rsq for `g*g`) that
  the SPIR-V surface cannot bind — `NoContraction` blocks FMA
  fusion only.

ADR body (above) remains frozen per ADR-0028. The decision to ship
Step A as a partial fix and leave Step B blocked stands; the
research-0089 digest expands the *runner-up* options (per-stage
NVIDIA dynamic dump, `places=3` override, driver-team escalation)
without amending the original decision matrix.

### Status update 2026-05-09: Phase 3 fix landed

Phase 3 (this PR) replaced all three bare `barrier()` calls in
`vif.comp` with explicit `memoryBarrierShared() + barrier()` pairs
(SPIR-V `OpControlBarrier` with `gl_StorageSemanticsShared |
gl_SemanticsAcquireRelease` shared-memory release-acquire). Real
hardware run on this session: NVIDIA RTX 4090 + driver 595.71.05 +
local API-1.4 bump now passes the `cross_backend_vif_diff.py
--places 4` gate 0/48 across all four scales, with 5-run
deterministic `(num_scale2, den_scale2)` matching the CPU
reference. RADV (Mesa 26.1.0) was already 0/48 pre-fix and stays
0/48 post-fix. The original Phase-2 attribution to "NVIDIA"
turned out to be off-by-one in the loader device map: PR #510's
`--vulkan_device 0` numbers landed on Intel Arc A380 (Mesa-ANV /
DG2), not NVIDIA — see research-0089 2026-05-09 appendix
"Hardware lane this session — corrected device map" for the
empirical proof.

A new residual moved into the Open queue as
**T-VK-VIF-1.4-RESIDUAL-ARC**: Arc A380 + ANV at API 1.4 still
exhibits the 45/48 scale-2 + non-deterministic int64 accumulator
pattern even with the `memoryBarrierShared()` pair in place.
Likely needs a stronger qualifier (`coherent`/`volatile` shared,
or `controlBarrier(gl_ScopeDevice, ...)`, or
`subgroupMemoryBarrierShared()` before the elected-thread write).
Tracked separately. Step B (`apiVersion` bump to 1.4) remains
blocked until the Arc residual closes; this Phase-3 fix is a
prerequisite, not a sufficient condition.

ADR body (above) remains frozen per ADR-0028. The original
decision to ship Step A as partial and defer Step B stands;
Phase-3 narrows the blocker scope from "vif residual at 1.4 on
NVIDIA + Mesa-ANV" to "vif residual at 1.4 on Mesa-ANV only".

### Status update 2026-05-09: Phase 3b — stronger-fence experiments + hardware-mapping correction

Phase 3b tested three stronger-fence candidates against `vif.comp`'s
Phase-4 cross-subgroup int64 reduction site, on top of PR #511's
workgroup-scope `memoryBarrierShared(); barrier();` baseline. **None
of the three candidates closes the residual.**

**Hardware-mapping correction.** Re-baselining at API 1.4 with PR #511
in place on this session's multi-GPU host (NVIDIA RTX 4090 + Intel
Arc A380 + AMD RADV/CPU) showed the Phase-3 attribution was inverted:

| device | hardware                         | gate result                          |
| ------ | -------------------------------- | ------------------------------------ |
| 0      | NVIDIA RTX 4090 + 595.71.05      | scale 2: **45/48 FAIL**, max 1.527e-02 |
| 1      | Intel Arc A380 + Mesa-ANV 26.1.0 | 0/48 OK on every scale               |
| 2      | AMD RADV (CPU) + Mesa 26.1.0     | 0/48 OK on every scale               |

`vmaf_vulkan_context_new`'s device sort is stable inside the same
`devtype_score` bucket and the `vkEnumeratePhysicalDevices` order is
host-policy-dependent (driver registration / Mesa device-select layer
/ env vars). On this host `--vulkan_device 0` is NVIDIA, not Arc. The
empirical 45/48 / `1.527e-02` / 5-run-non-deterministic signature is
real; the device-name tag PR #511 attached to it was wrong. The
state.md row originally opened as `T-VK-VIF-1.4-RESIDUAL-ARC` is
therefore tracking a phantom — Arc is already clean — and is replaced
by `T-VK-VIF-1.4-RESIDUAL-NVIDIA-DEFERRED`.

**Candidate results (research-0090 §"Candidates tested"):**

- **C1** (`shared coherent` / `shared volatile`) — _not buildable_.
  glslc 2026.1 rejects with "memory qualifiers cannot be used on
  this type"; per GLSL 4.50 §4.10, those qualifiers apply to
  buffer + image variables only, not `shared`.
- **C2** (`subgroupMemoryBarrierShared()` before the workgroup-scope
  pair) — _builds, no effect_. NVIDIA still 45/48 FAIL scale 2.
- **C3** (device-scope `controlBarrier(gl_ScopeWorkgroup, gl_ScopeDevice,
  gl_StorageSemanticsShared, gl_SemanticsAcquireRelease)`) — _builds,
  no effect_. NVIDIA still 45/48 FAIL scale 2, 5-run still non-deterministic.
- **C2 + C3 stacked** — same 45/48 FAIL.

**Working hypothesis.** The ~10^14 magnitudes with sign flips on `den`
are not consistent with reading uninitialised lanes from shared memory
(stronger fences would close those). They are more consistent with a
driver bug in NVIDIA's int64 emulation of `subgroupAdd(int64_t)` for
SCALE=2's specific subgroup-size schedule — the SCALE=2 specialisation
has the smallest `valid` thread fraction, which would exercise an
inactive-lane path in the driver's int64 reduction lowering. Confirming
the hypothesis needs a manual lane-by-lane subgroup reduction over
int32 halves (or an int64 `subgroupShuffleXor` butterfly). That patch
is out of scope for Phase 3b — the brief enumerated three fence
candidates and stopped after them — and is tracked as the next-step
of the deferral row.

**No relaxation.** Per `feedback_no_test_weakening`, the `places=4`
gate stays at `places=4` and the API-1.4 bump (Step B) remains
blocked. The shipping default is API 1.3 where the gate is **0/48 on
every device** (NVIDIA + Arc + RADV all verified end-of-session).
Step B unblocks when (a) a manual int64 subgroup-reduction patch
closes NVIDIA + Arc + RADV at 0/48 5-run-deterministic at API 1.4
with no perf regression on Arc/RADV, OR (b) NVIDIA ships a driver
release that closes the residual.

### Status update 2026-05-14: Phase 3c — manual int64 subgroup reduction closes VIF residual

Phase 3c replaces `vif.comp`'s seven Phase-4 `subgroupAdd(int64_t)`
calls with an explicit `subgroupShuffleXor` butterfly helper. The
shader now requires `GL_KHR_shader_subgroup_shuffle`; host descriptors,
push constants, accumulator layout, and CPU/CUDA/SYCL score contracts
are unchanged.

Local API-1.4 validation on the same three-device host used in Phase
3b closes the residual:

| device | hardware                         | gate result after Phase 3c |
| ------ | -------------------------------- | -------------------------- |
| 0      | NVIDIA RTX 4090 + 595.71.05      | 0/48 OK on every scale; scale 2 max `2.000000e-06` |
| 1      | Intel Arc A380 + Mesa-ANV 26.1.0 | 0/48 OK on every scale; scale 2 max `2.000000e-06` |
| 2      | AMD RADV (CPU) + Mesa 26.1.0     | 0/48 OK on every scale; scale 2 max `2.000000e-06` |

The NVIDIA gate was repeated five times and stayed 0/48 on every run.
This confirms the Research-0090 working hypothesis sufficiently for
the fork: the remaining VIF blocker was NVIDIA's int64 subgroup-add
lowering path, not the shared-memory fences. Rebase invariant:
`vif.comp`'s Phase-4 accumulator path must keep the manual int64
shuffle reduction and must not be simplified back to
`subgroupAdd(int64_t)`.

The ciede2000 f32/f64 precision tail remains separately documented
under ADR-0273; Phase 3c only closes the VIF half of the API-1.4
blocker.

ADR body (above) remains frozen per ADR-0028.
