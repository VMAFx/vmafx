<!-- markdownlint-disable MD013 MD060 -->
# ADR-0391: ciede2000 Vulkan NVIDIA places=4 fork debt is a structural f32/f64 precision gap

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: vulkan, ciede, precision, gpu, nvidia, fork-local

## Context

PR #346 ("vif + ciede shaders — precise decorations") cut ciede2000's
NVIDIA-Vulkan places=4 cross-backend mismatch from 42/48 to 5/48 frames
(max abs `1.67e-04` → `8.9e-05`) by tagging the load-bearing FP ops in
`ciede.comp` with GLSL `precise`. The remaining 5/48 tail at 1.78× the
places=4 threshold (`5.0e-05`) was deferred there as "CPU-side
double-vs-float bisect follow-up."

This ADR closes that follow-up. The investigation (see
[research-0055](../research/0055-ciede-vulkan-nvidia-f32-f64-root-cause.md))
ran a controlled experiment: rebuild the CPU `ciede.c::get_lab_color`
to do its colour-space chain in `float` instead of `double` (matching
the Vulkan shader's precision contract), then diff against the NVIDIA
RTX 4090 (driver 595.71.05) Vulkan output. Result: float-CPU and
NVIDIA-Vulkan agree to ~6e-7 on the 5 frames that fail double-CPU vs
NVIDIA-Vulkan at places=4 (frames 0/1/2/5/6 — the highest-ΔE frames in
the 48-frame fixture). Conversely, on the 43 frames that pass
double-CPU vs NVIDIA-Vulkan, float-CPU vs NVIDIA-Vulkan diverges by
~8.5e-5 (i.e. *worse* than the gate). The signal is unambiguous: the
shader is doing math correctly; the CPU reference is doing math in
double and the residual gap is the irreducible f32 vs f64 precision
delta on the highest-ΔE pixels, where the per-pixel ΔE sum amplifies
single-precision rounding.

The CPU reference's `get_lab_color()` takes `double` arguments and
runs the BT.709 → linear-RGB → XYZ → Lab chain in `double`, narrowing
to `float` only on assignment to the `LABColor` struct. The Vulkan
shader, like every fork GPU backend, is float32 throughout. Vulkan's
`shaderFloat64` device feature is optional (not present on most
consumer GPUs at full throughput — RTX 4090 reports it but at 1/64 of
fp32 throughput; lavapipe supports it without penalty but lavapipe is
already passing places=4) and would require dual-pipeline compilation
to keep the lavapipe lane fast. Even if `shaderFloat64` were used, the
SPIR-V spec doesn't mandate IEEE-754 conformance for f64 transcendentals
— another driver-divergence vector.

## Decision

We accept the 5/48 NVIDIA-Vulkan ciede2000 places=4 mismatch as a
structural fork debt and document it under
[`docs/state.md`](../state.md) Open bugs as **T-VK-CIEDE-F32-F64**. We
do not promote the ciede shader to f64. The lavapipe parity gate
(places=4, currently 0/48) remains authoritative for CI; NVIDIA
hardware validation stays a manual local gate documented in
[`docs/backends/vulkan.md`](../backends/vulkan.md). PR #346's `precise`
decorations stay as the high-water mark of what shader-level
mitigation can achieve on f32; further work on this tail requires
either f64 promotion (rejected here) or f32-remediation in the CPU
reference (out of scope — the CPU reference is the source of truth and
the Netflix golden gate is built around its current f32/f64 mix).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Promote ciede shader to f64 with `shaderFloat64` device feature | Closes the gap on hardware that supports it (RTX 4090 does, lavapipe does, Mesa anv does) | Optional Vulkan feature — not portable. RTX 4090 runs f64 at 1/64 fp32 throughput (~64× slower per-pixel). SPIR-V f64 transcendentals (`Pow`, `Sqrt`, `Sin`) are not bit-mandated by spec — driver divergence still possible. Would require dual-pipeline (f32 lavapipe / f64 NVIDIA) to keep lavapipe fast | Complexity + perf cost greatly outweighs the 5/48 tail at 1.78× threshold |
| Add CPU-side f32 fast path under a flag, ship f32-CPU as the fork's reference for ciede2000 | Trivially closes the GPU gap on every backend | Breaks the 8-year-old CPU reference behaviour. Netflix golden gate built around current f32/f64 mix. Diverges from upstream Netflix/vmaf master (rebase debt) | Out of scope — CPU reference is source-of-truth; we don't change it to make the GPU look good |
| Polynomial approximation of `pow(x, 2.4)` and `pow(x, 1/3)` matched to glibc's f64 implementation, evaluated in f32 with Kahan summation | Closes the gap purely shader-side, no f64, no CPU change | Substantial engineering effort to match glibc's specific transcendental implementation; not portable across Vulkan drivers (lavapipe vs NVIDIA `Pow` SPIR-V lowerings differ); 5/48 tail doesn't justify the complexity | Cost-benefit fails — places=4 threshold at 1.78× is acceptable as documented fork debt |
| **Accept as documented fork debt; lavapipe gate authoritative** (this ADR) | Zero engineering cost. lavapipe passes places=4 (0/48 mismatches). NVIDIA gap is bounded (1.78× threshold, 5/48 frames). Bug entry in `docs/state.md` keeps it visible | NVIDIA-hardware users see the gap on local cross-backend diff; mitigated by docs in `docs/backends/vulkan.md` | Chosen — proportional response to a structural precision issue with no clean shader-level fix |

## Consequences

- **Positive**: closes the PR #346 deferred follow-up with a definitive
  root cause; replaces "5/48 mystery tail" with "irreducible f32/f64
  gap on high-ΔE frames"; future investigators don't redo the
  experiment.
- **Negative**: NVIDIA-hardware users running the local
  `cross_backend_vif_diff.py --feature ciede --backend vulkan` see 5
  failures on the 576×324 fixture. Mitigated by an entry in
  [`docs/backends/vulkan.md`](../backends/vulkan.md) calling this out.
- **Neutral / follow-ups**: T-VK-CIEDE-F32-F64 row added to
  `docs/state.md` Open bugs; will close if a future ADR enables
  optional f64 path (e.g. for an HDR-Lab use case where f64 is already
  required) or if the CI parity matrix grows an NVIDIA hardware lane
  via self-hosted runner — that lane would then need a per-feature
  tolerance override (places=3 for ciede on NVIDIA) to be green.

## References

- PR #346 ("vif + ciede shaders — precise decorations") commit message
  reservation: "deferred to a CPU-side double-vs-float bisect
  follow-up"
- [ADR-0187](0187-ciede-vulkan.md) — original ciede Vulkan kernel
- [ADR-0265](0265-vif-ciede-precise-step-a.md) (in PR #346, not yet
  merged) — `precise` decorations rationale
- [research-0055](../research/0055-ciede-vulkan-nvidia-f32-f64-root-cause.md)
  — full experimental data
- [`docs/state.md`](../state.md) Open bugs — T-VK-CIEDE-F32-F64
- Source: `req` — "investigate this pre-existing places=4 violation
  on NVIDIA driver, root-cause it, and either ship a fix OR file it
  as a tracked Open bug"
