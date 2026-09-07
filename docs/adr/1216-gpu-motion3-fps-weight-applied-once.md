<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1216: The GPU motion3 twins apply `motion_fps_weight` exactly once

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing`

## Context

`motion_fps_weight` is a `VMAF_OPT_FLAG_FEATURE_PARAM` option on the integer
motion extractor: an fps-aware multiplicative correction applied to the
SAD-derived motion score. The CPU reference applies it in exactly one place —
`extract()` in [`core/src/feature/integer_motion.c`](../../core/src/feature/integer_motion.c),
where the per-frame SAD is scaled and clipped before being written to the
feature collector as `motion_sad_score`. Everything downstream reads that
already-weighted value back: `flush()` derives `motion2` as a min over
neighbouring `motion_sad_score` entries and then blends `motion2` into
`motion3` through `motion_blend()` with no further weighting.

The CUDA, SYCL and HIP twins each carry a host-side `motion3_postprocess_*()`
helper that reproduced this blend. All three opened with
`score2 * s->motion_fps_weight` — but every caller in all three twins already
passes a value that has been fps-weighted *and* `motion_max_val`-clipped. The
weight was therefore applied twice, so `motion3` carried `motion_fps_weight`
squared.

The defect was invisible to the existing gates because `motion_fps_weight`
defaults to `1.0`, and `1.0² = 1.0`. Every motion3 parity test in the tree ran
the extractors with `NULL` options, so all three twins agreed with the CPU on a
value neither of them was computing correctly. This is the same blind-spot
shape as ADR-1204 / ADR-1206: a parity test that pins the one parameter that
would expose the drift.

## Decision

We will remove the second `motion_fps_weight` multiplication from
`motion3_postprocess_cuda()`, `motion3_postprocess_sycl()` and
`motion3_postprocess_hip()`, blending the caller-supplied `score2` directly, so
each twin applies the weight exactly once — at the same point the CPU reference
does. Each of the three motion3 parity tests gains a variant that pins
`motion_fps_weight = 0.6` and asserts CPU/GPU parity on the derived
`integer_motion3_mfw_0.6` key.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Remove the weighting from `motion3_postprocess_*` (chosen) | One-line change per twin; the surviving application sits exactly where the CPU applies it; callers stay untouched | None material | — |
| Strip the weight from the callers and keep it inside `motion3_postprocess_*` | Also yields a single application for motion3 | The callers' weighted value is *also* the `motion2` emission, which must stay weighted; stripping it would break motion2 to fix motion3 | Trades one bug for another |
| Divide the weight back out inside `motion3_postprocess_*` | Smallest textual diff | Introduces a division by an option whose declared `min` is `0.0`; a `motion_fps_weight = 0` run would produce NaN instead of `0` | Numerically unsafe |
| Document the divergence and leave the twins as they are | Zero code risk | A non-default `motion_fps_weight` silently returns a different metric on GPU than on CPU — the fork's cross-backend parity contract (ADR-0214) forbids exactly this | Contradicts the parity contract |

## Consequences

- **Positive**: `motion3` agrees across CPU / CUDA / SYCL / HIP for every legal
  `motion_fps_weight`, not just the default. The three new parity variants keep
  it that way.
- **Negative**: any downstream consumer that had calibrated around the squared
  weight on a GPU backend will see `motion3` change. No in-tree model or
  snapshot does — every shipped model leaves `motion_fps_weight` at its `1.0`
  default, where the arithmetic is unchanged.
- **Neutral / follow-ups**: the Metal twin has no integer-motion3
  post-process to fix (it implements `motion_v2`, whose weighting is a single
  application at the emission site). `float_motion` on every backend already
  applies the weight once, at the emission site — verified while auditing this
  defect, no change needed.

## References

- Finding id 122/123/124 from the twin-drift sweep: `motion3_postprocess_cuda`
  multiplies its input by `motion_fps_weight` again, but every caller already
  passes a value that has been fps-weighted and clipped, so `motion3_score`
  carries the weight squared. The CPU reference applies `motion_fps_weight`
  exactly once, in `extract()`.
- Measured drift on the RTX 4090 before the fix, 256x144 8-bpc fixture,
  `motion_fps_weight = 0.6`: `cpu = 14.48987751`, `cuda = 8.69392654`
  (`= 14.48987751 × 0.6`), delta `5.80e+00` against a `1e-4` tolerance.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity CI gate and its
  places=4 cross-backend tolerance.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — the derived
  feature-name key (`integer_motion3_mfw_0.6`) the parity variants read.
- [ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md),
  [ADR-1206](1206-gpu-parity-large-fixture-variants.md) — the same
  pinned-fixture blind-spot pattern.
