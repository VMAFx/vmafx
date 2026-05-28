# ADR-0716: Drop ansnr / float_ansnr and speed_temporal / speed_chroma (−7.4k LOC, zero AI value)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `build`, `feature`, `ai`, `vmafx`

## Context

Research-0733 (feature importance audit, PR #25) evaluated every feature
extractor in the fork against two criteria:

1. **Permutation importance** across all shipped tiny-AI models (ONNX).
2. **Standalone MOS correlation** (|PLCC|) on the held-out evaluation set.

Two feature groups scored zero permutation importance on all models and
exhibited near-zero standalone MOS correlation:

| Feature group | LOC | Permutation importance | |MOS PLCC| |
|---|---|---|---|
| `ansnr` / `float_ansnr` | ~1,626 | 0 across all models | ~0.17 |
| `speed_temporal` + `speed_chroma_{u,v,uv}` | ~5,783 | 0 across all models | 0.06–0.09 |

Both feature groups accumulated GPU coverage (CUDA, SYCL, HIP, Vulkan,
Metal) that must now be maintained across every backend promotion wave.
The combination of zero AI value and high maintenance cost makes retention
unjustifiable under the VMAFX Phase 4b modernization effort.

The `vmaf_v0.6.1.json` model does not reference either feature group (it
uses `adm2`, `motion2`, and `vif_scale[0-3]`), so the Netflix golden-data
gate is unaffected.

## Decision

Drop `ansnr` / `float_ansnr` and `speed_temporal` / `speed_chroma_u/v/uv`
from the fork entirely: delete C sources, SIMD paths, GPU backend TUs and
shaders, meson.build entries, test binaries, Python harness classes, and
user-facing documentation.

Retain `speed_qa` — the NR SpEED-QA extractor is a separate implementation
with different purpose (no float dependency, different algorithm) and is not
part of this audit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Retain as opt-in (`--feature float_ansnr`) | Zero code churn | Ongoing GPU-backend maintenance; zero AI value | Opt-in features still require full build and test coverage |
| Stub to -ENOSYS | Removes runtime overhead | Same build surface; confusion for users who request the feature by name | NOLINT-or-ENOSYS is a code smell for features with zero value |
| Drop CPU only, keep GPU | Reduces CPU build debt | GPU backends orphaned without CPU reference | No benefit; GPU kernels are more maintenance-intensive |

## Consequences

- **Positive**: ~7,400 LOC removed; CUDA/SYCL/HIP/Vulkan/Metal build surfaces
  shrink; no NOLINT-cover maintenance needed; CI time reduced slightly.
- **Negative**: `VmafLegacyQualityRunner` (Python SVM path) now scores with 3
  features instead of 4 — the legacy SVM model was trained with ansnr so
  prediction quality degrades. This runner is deprecated; operators should use
  the JSON model path.
- **Neutral / follow-ups**: Any upstream Netflix/vmaf sync that touches
  `ansnr` or `speed_temporal`/`speed_chroma` TUs must drop those hunks.
  See `docs/rebase-notes.md` for the canonical warning.

## References

- Research-0733 (feature importance audit, PR #25)
- ADR-0709 (Phase 4b umbrella)
- req: The user directed removal of these features based on the audit results,
  citing zero AI importance and |MOS PLCC| of 0.06–0.17 as justification.
