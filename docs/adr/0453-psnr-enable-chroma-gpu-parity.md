<!-- markdownlint-disable MD060 -->
# ADR-0453: PSNR `enable_chroma` option parity across all GPU backends

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: cuda, sycl, vulkan, psnr, option-parity, bug

## Context

The feature-option parity audit of 2026-05-16 found that the three GPU
PSNR extractors (`psnr_cuda`, `psnr_sycl`, `psnr_vulkan`) exposed an
empty `VmafOption options[]` table. The CPU reference extractor
(`integer_psnr.c`) exposes `enable_chroma` (default `true`), which
controls whether chroma planes are computed and whether `psnr_cb` /
`psnr_cr` are emitted.

When a caller passed `enable_chroma=false` to a GPU extractor, the
unknown-option path silently dropped the flag, the GPU extractor
continued emitting three-plane PSNR, and the JSON output diverged from
the CPU reference which correctly emitted only `psnr_y`. The divergence
was silent — no warning, no error.

This constitutes a correctness regression in any pipeline that uses GPU
PSNR with `enable_chroma=false` on non-YUV400 sources.

The PR #880 pattern (adding `motion_fps_weight` to GPU motion twins)
provides the precedent for this class of fix.

See Research-0136 for the full finding and fix-shape analysis.

## Decision

For each of the three GPU twins (`integer_psnr_cuda.c`,
`integer_psnr_sycl.cpp`, `psnr_vulkan.c`):

1. Add `bool enable_chroma` to the state struct.
2. Add a matching `VmafOption` entry (name, help, offset, type,
   `default_val.b = true`) to the options table.
3. In `init()`, after the existing YUV400 `n_planes = 1` guard, add a
   second guard that clamps `n_planes` to 1 when `enable_chroma == false`.

No kernel code, readback logic, or score-emission code is changed. The
change is purely in the option-dispatch and init-geometry paths. Bit-
exactness at the default `enable_chroma=true` is guaranteed by
construction — `n_planes` resolves to the same value as before on all
non-YUV400 sources.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-backend opt-out stub returning `-ENOTSUP` | Simple, no GPU geometry change | Silences the option rather than honouring it; caller still sees divergent output | Masks the bug rather than fixing it |
| Add all five CPU options at once (`enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse`) | Closes more parity gaps in one PR | Wider scope; `enable_apsnr` needs temporal accumulation buffers on the GPU that are not yet in place; `min_sse` needs per-plane max recomputation | Defer multi-option expansion to follow-up PRs per the SCOPE GUARD |

## Consequences

- **Positive**: `enable_chroma=false` now produces identical `psnr_y`-only
  JSON output on CPU and all three GPU backends. Cross-backend parity gate
  passes at `places=4` for both `true` and `false` settings.
- **Negative**: None. The default path is bit-for-bit unchanged.
- **Neutral / follow-ups**: The remaining four CPU options (`enable_mse`,
  `enable_apsnr`, `reduced_hbd_peak`, `min_sse`) are still absent from the
  GPU twins. They are tracked in the feature-option parity audit backlog as
  P2 items.

## References

- CPU reference: `core/src/feature/integer_psnr.c` lines 58–96 (options
  table) and lines 123–125 (init guard).
- [Research-0136](../research/0136-psnr-enable-chroma-cross-backend-2026-05-16.md)
- [ADR-0216](0216-vulkan-chroma-psnr.md) — Vulkan chroma extension
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU-parity CI gate
- PR #880 — motion_fps_weight option-parity precedent
- req: user brief 2026-05-16 ("extend integer_psnr.enable_chroma to ALL GPU
  twins … mirror the PR #880 pattern")
