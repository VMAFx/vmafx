# Research Digest: integer_motion pipelined rename (upstream a4a1492d3)

**Port date:** 2026-06-01
**Upstream author:** Kyle Swanson, Netflix
**Upstream SHA:** a4a1492d3e5c9e2cc10549273cd19d248c6e9bea

## Summary

Upstream Netflix/vmaf replaced the two-pass `integer_motion` extractor
(which stored 3-5 blurred frames in a circular buffer) with the
single-pass pipelined algorithm originally prototyped as `integer_motion_v2`.
The pipelined approach exploits the linearity of convolution:
`SAD(blur[N-1], blur[N]) == SAD(blur(f[N-1] - f[N]))`, fusing the diff,
y-convolution, and row-absolute-sum into one pass over a single scratch row.

## Key design points

1. **Flag change**: `VMAF_FEATURE_EXTRACTOR_TEMPORAL` to
   `VMAF_FEATURE_EXTRACTOR_PREV_REF`. The framework now provides the
   previous reference frame via `fex->prev_ref` (and `fex->prev_prev_ref`
   for the five-frame window).
2. **Flush-based scoring**: motion2 / motion3 scores are computed in
   `flush()` from a series of raw SAD values accumulated during
   `extract()`, enabling correct retroactive stamping of frame 0.
3. **SIMD**: `motion_avx2/avx512.{c,h}` now implement the pipeline
   functions `motion_score_pipeline_{8,16}_avx2/avx512`. The
   `motion_v2_avx2/avx512` files that the fork adds remain present
   (used by GPU backends).

## Fork-local impact

- `vmaf_fex_integer_motion_v2` (CPU) removed from
  `feature_extractor_list[]`.
- GPU backends (`_cuda`, `_sycl`, `_hip`, `_metal`) expose
  `integer_motion_v2_*` names; those are not changed here.
- `integer_motion_v2.c` and `motion_v2_avx2/512.{c,h}` retained for
  GPU paths.
- Upstream precision reductions in `python/test/` golden assertions
  NOT applied (CLAUDE.md §12 r1).

## Numeric equivalence

Netflix's commit states the new implementation matches the v2 output to
within `places=5` (matching upstream's own relaxed assertions). The fork's
CI golden gate (`python/test/`) retains `places=8` and `places=4` as
before; if CI shows drift, the root cause should be investigated rather
than loosening thresholds.
