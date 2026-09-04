<!-- markdownlint-disable MD013 MD029 MD038 MD041 MD060 -->
# ADR-1175: Opt-in `uncapped` option for PSNR feature extractors (T-UPSTREAM-1109 / Netflix#1109)

- **Status**: Accepted
- **Date**: 2026-09-04
- **Deciders**: Lusoris, Claude (Anthropic), Antigravity
- **Tags**: core, metrics, psnr, options, cross-backend

## Context

In upstream libvmaf, `integer_psnr.c` and `float_psnr.c` cap the per-frame PSNR
score unconditionally at `(6 * bpc) + 12` dB (60 dB for 8-bit, 72 dB for 10-bit,
84 dB for 12-bit, 108 dB for 16-bit). Specifically, `integer_psnr.c:203` and `:240`
evaluate `MIN(10. * log10(peak * peak / MAX(mse, 1e-16)), s->psnr_max[p])`.

As reported in Netflix/vmaf#1109 and tracked in `docs/state.md` under
`T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03`, this hard saturation causes any
genuinely computed near-lossless PSNR values above the cap (for example, a single
luma sample delta on a 576×324 frame gives MSE = 1/186624, yielding a ground-truth
PSNR of 100.840479 dB) to be truncated to 60.0 dB. All GPU twins
(`integer_psnr_cuda.c`, `float_psnr_cuda.c`, `integer_psnr_sycl.cpp`,
`float_psnr_sycl.cpp`, `integer_psnr_hip.c`, `float_psnr_hip.c`,
`integer_psnr_metal.mm`, and `float_psnr_metal.mm`) mirror this unconditional clamp.

While the `min_sse` option already exists in `integer_psnr.c` to raise the PSNR
ceiling, it was designed primarily to bound minimum MSE for identical frames rather
than to decouple infinity-sentinel handling from true finite PSNR calculation.
Furthermore, modifying the default behavior would violate the Netflix golden-data
gate (CLAUDE.md §12 r1), which enforces exact agreement on byte-identical
test pairs (`src01_hrc00_576x324.yuv` ↔ `src01_hrc00_576x324.yuv`, etc.) at 60.0,
84.0, and 108.0 dB.

## Decision

We introduce an opt-in boolean option `uncapped` (default `false`) across CPU
(`integer_psnr.c`, `float_psnr.c`) and all GPU twins (`cuda`, `sycl`, `hip`, `metal`):

1. **Option declaration**: Add `{ .name = "uncapped", .type = VMAF_OPT_TYPE_BOOL, .default_val.b = false }`
   to the options table of `integer_psnr.c`, `float_psnr.c`, and all GPU twins.
2. **Scoring semantics**: Compute PSNR with an explicit `sse == 0` sentinel:

   ```c
   psnr = (sse == 0) ? s->psnr_max[p] : 10.0 * log10(peak * peak / mse);
   if (!s->uncapped)
       psnr = MIN(psnr, s->psnr_max[p]);
   ```

   For byte-identical frames (`sse == 0`), `psnr` evaluates to `psnr_max[p]` in
   both capped and uncapped modes. For non-identical frames with `sse > 0`, when
   `uncapped=true`, the true finite PSNR value is reported without truncation.
   When `uncapped=false` (the default), `psnr` is clamped at `psnr_max[p]`,
   preserving exact bit-identical compatibility with Netflix/vmaf.
3. **Cross-backend consistency**: All GPU twins parse `uncapped` through the
   standard option dictionary mechanism (ADR-0453 pattern) and wrap their
   respective clamp checks in `if (!s->uncapped)`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Change default behavior to uncapped | Natural mathematical definition; no flags needed | Breaks Netflix golden assertions (which expect 60/84/108 dB) and downstream scripts expecting saturation | Violates CLAUDE.md §12 r1 and breaks backward compatibility |
| Raise the cap (e.g. to 120 or 200 dB) | Allows higher near-lossless dynamic range | Arbitrary constant; still truncates valid values; shifts behavior unexpectedly | Still a cap; doesn't solve the underlying truncation flaw |
| Opt-in `uncapped` boolean option (Chosen) | 100% bit-identical default preserving golden data; true mathematical PSNR when opted in; uniform across all 5 backends | Requires opting in via `--feature psnr=uncapped=true` | Best balance of correctness, safety, and backward compatibility |

## Consequences

- **Positive**: Users analyzing high-fidelity and near-lossless video can retrieve
  exact PSNR scores above 60/72/84/108 dB matching FFmpeg `psnr` filter output.
  Default behavior remains completely unchanged, guaranteeing zero drift on the
  Netflix golden gate.
- **Negative**: Adds one boolean field to the option table and state structs across
  PSNR extractors.
- **Neutral / follow-ups**: Documented in `docs/metrics/psnr.md` and linked from
  `docs/metrics/features.md`. Unit tests added in `test_integer_psnr_coverage.c`.

## References

- Netflix Issue: [Netflix/vmaf#1109](https://github.com/Netflix/vmaf/issues/1109)
- Tracking item: `T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03` in `docs/state.md`
- [ADR-0108](0108-deep-dive-deliverables-rule.md) (deep-dive deliverables)
- [ADR-0453](0453-psnr-enable-chroma-gpu-parity.md) (PSNR enable_chroma GPU parity)
- [ADR-1033](1033-cpu-scoring-nan-ub-guards.md) (CPU-side scoring NaN/UB guards)
- Source: `req` (user prompt specification)
