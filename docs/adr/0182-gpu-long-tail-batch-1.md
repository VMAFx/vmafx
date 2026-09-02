# ADR-0182: GPU long-tail batch 1 — psnr + ciede + moment on CUDA / SYCL / Vulkan

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, cuda, sycl, vulkan, feature-extractor, fork-local

## Context

PR #124 (T7-26 / ADR-0181) landed the global feature-characteristics
registry + per-backend `dispatch_strategy` modules. The
[`metrics-backends-matrix`](../../.workingdir2/analysis/metrics-backends-matrix.md)
GPU columns now show that 14 of ~16 registered metrics are missing
GPU coverage — only `vif`, `motion`, and `adm` ship on CUDA / SYCL
/ Vulkan today.

To start closing that long tail without writing 14 PRs, this PR
bundles the **3 simplest GPU-friendly metrics** in one batch:
**psnr**, **ciede**, **moment**. Each is a per-pixel kernel with
either a single reduction (psnr, moment) or no reduction at all
(ciede). They share the same scaffolding pattern as the existing
GPU `vif`/`motion` ports. This is the validation pass for the
registry-driven dispatch architecture before tackling the more
complex metrics (ssim, ms_ssim, ssimulacra2, cambi, psnr_hvs).

## Decision

We add 9 new `VmafFeatureExtractor` registrations in one PR:

| Metric | CUDA | SYCL | Vulkan |
| --- | --- | --- | --- |
| `psnr` | new | new | new |
| `ciede` | new | new | new |
| `float_moment` | new | new | new |

Each registration:

- New extractor registration (`vmaf_fex_<name>_<backend>`) +
  matching `set_fex_<backend>_state()` wiring in `libvmaf.c`.
- New kernel under
  `core/src/feature/<backend>/<metric>_<backend>.{c,cpp,cu}`.
- Vulkan: new GLSL compute shader under
  `core/src/feature/vulkan/shaders/<metric>.comp`.
- Cross-backend gate extension:
  `scripts/ci/cross_backend_vif_diff.py` gains
  `--feature {psnr,ciede,moment}` selectors.
- Per-feature descriptor seeded on the existing scalar
  registration (`psnr.c`, `ciede.c`, `float_moment.c`) so the
  registry's dispatch decision applies uniformly.

**Scope ordering**: psnr Vulkan → psnr CUDA → psnr SYCL →
ciede {Vulkan, CUDA, SYCL} → moment {Vulkan, CUDA, SYCL}. Each
backend group lands as a separate commit on the feature branch
so a partial revert is cheap if a backend regresses.

**Bit-exactness contract**: same as the existing GPU ports —
`places=4` cross-backend gate vs CPU scalar reference, verified
by the lavapipe CI lane on every PR. Vulkan kernels target
`int64` accumulators (`GL_EXT_shader_explicit_arithmetic_types_int64`)
for deterministic reductions.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| One PR per metric × backend (9 PRs) | Smaller review diffs | 9× CI round-trip; copy-paste of meson + dispatcher wiring across 9 PRs; cross-backend gate touched 9 times | Bundle wins on overhead; rollback granularity preserved via per-backend commits on the same branch |
| One PR per metric (3 PRs, all backends each) | Logical "one metric = one PR" granularity | Still 3× the meson + cross-backend-gate churn; the registry plumbing is identical across metrics so 3 PRs would touch the same files thrice | Bundle is cleaner once the pattern is proven by the first metric (psnr) within the same PR |
| Defer ciede or moment to a later batch | Smaller initial PR | The 3 metrics are mechanically the simplest; deferring the simple ones means the next batch tackles ssim/ms_ssim/ssimulacra2 with less proof-of-pattern under load | Bundle the 3 simple together as the validation pass, then take complexity bigger in batch 2 |
| Skip Vulkan in batch 1, do CUDA + SYCL only | Less infrastructure (no GLSL shader files) | Vulkan is the path with the active cross-backend lavapipe gate; landing CUDA / SYCL without Vulkan would mean 2 of 3 backends land without honest cross-backend numerical verification | Vulkan must be in batch 1 because the gate is keyed on it |

## Consequences

- **Positive**: 3 of the 14 missing metrics gain full GPU coverage
  in one PR. Cross-backend gate extends trivially. Registry-
  driven dispatch validates under load (3 metrics × 3 backends).
  Pattern proven, ready to scale to ssim / ssimulacra2 / cambi
  in batch 2.
- **Negative**: large diff (~6 000–8 000 LOC across 9 kernel
  TUs + GLSL shaders + meson plumbing + tests). One CI cycle to
  validate everything together; if one backend regresses, the
  whole bundle waits.
- **Neutral / follow-ups**:
  1. **Batch 2 (next PR)**: ssim + ms_ssim on CUDA / SYCL /
     Vulkan. SSIM has 4-5 dispatches/frame; benefits more from
     the registry's graph-replay decision.
  2. **Batch 3**: ssimulacra2 GPU port (T7-25). Multi-scale
     Gaussian pyramid; biggest single-metric PR.
  3. **Batch 4**: cambi + psnr_hvs (the trickier metrics —
     range-reduction histograms, 8×8 DCT respectively).
  4. **Batch 5**: ansnr + motion_v2 (variants of existing GPU
     metrics; should be quick).

## References

- Source: user direction 2026-04-26 (after PR #124 merged: "go
  on" + matrix shows 14 GPU gaps).
- Backlog: T7-23 (psnr Vulkan), T7-24 (ssim Vulkan), T7-25
  (ssimulacra2 GPU port). T7-23 is closed by this batch; T7-24
  and T7-25 remain for batches 2 and 3.
- Pattern parent: [ADR-0181](0181-feature-characteristics-registry.md)
  (registry + dispatch_strategy);
  [ADR-0177](0177-vulkan-motion-kernel.md) (Vulkan motion
  kernel — closest pattern for psnr/moment reductions);
  [ADR-0178](0178-vulkan-adm-kernel.md) (Vulkan ADM kernel —
  pattern for multi-dispatch features when batch 2 lands ssim).
- Matrix: [`.workingdir2/analysis/metrics-backends-matrix.md`](../../.workingdir2/analysis/metrics-backends-matrix.md).
