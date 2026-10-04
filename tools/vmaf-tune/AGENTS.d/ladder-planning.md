---
paths:
  - tools/vmaf-tune/src/vmaftune/ladder.py
  - tools/vmaf-tune/tests/test_ladder*.py
invariant: Ladder math is two-pass and order-sensitive; default 5-point CRF sampler; uncertainty is post-hull / pre-knee.
---
<!-- markdownlint-disable MD024 -->
# Bitrate ladder planning and sampling

- **`DEFAULT_SAMPLER_CRF_SWEEP` must stay inside every shipped
  adapter's `quality_range`.** Canonical 5-point sweep
  `(20, 25, 30, 35, 40)` is used by `_default_sampler` (called when
  `build_ladder(sampler=None)`); `corpus.iter_rows` runs
  `adapter.validate(preset, crf)` on every cell before encoding
  starts, so sweep point below any adapter's lower bound (e.g.
  `SvtAv1Adapter.quality_range = (20, 50)`) raises `ValueError`
  pre-encode and ladder exits 2. Lower bound 20 is *maximum* of
  every shipped adapter's lower bound — bumping it further is fine;
  lowering it requires either widening every adapter's
  `quality_range` or per-adapter default sweeps. Bug N-2 regression
  covered by `tests/test_ladder_svtav1_default_crf.py`.
- **Ladder uncertainty is post-hull / pre-knee.** `vmaf-tune ladder
  --with-uncertainty` must run ADR-0279 prune/insert recipe only
  after `convex_hull()` and before `select_knees()`. Preserve
  corpus row `vmaf_interval` payloads when present; when rows are
  point-only, use active `wide_interval_min_width` as conservative
  centred fallback interval so point-only corpora still
  participate in midpoint insertion.
- **Phase E ladder math is two-pass and order-sensitive.**
  `convex_hull` in `ladder.py` runs (1) Pareto filter sorted by
  bitrate ascending, vmaf descending tie-break; (2) upper-convex
  envelope with `cross >= 0` pop predicate (drops
  accelerating-returns interior points so hull is concave /
  diminishing-returns end-to-end). Re-deriving hull from different
  starting condition is easy to get subtly wrong — algorithm is
  pinned by `test_ladder.py` invariants (monotonic both axes, no
  domination). Don't refactor without re-running that suite.
- **Phase E spacing names are part of CLI contract.** `--spacing
  log_bitrate` is the default, `--spacing vmaf` is the documented
  perceptual-spacing mode, and `uniform` is legacy alias for
  `vmaf`. Keep CLI choices and `ladder.select_knees()` aliases in
  lockstep so argparse cannot accept value library rejects.
- **Phase E sampler is pluggable; default is 5-point CRF sweep
  (ADR-0307).** `ladder.build_ladder` accepts explicit `sampler=`
  callback; when omitted, `_default_sampler` composes
  `corpus.iter_rows` (Phase A encode+score) with
  `recommend.pick_target_vmaf` (smallest CRF clearing target VMAF)
  over canonical sweep
  `DEFAULT_SAMPLER_CRF_SWEEP = (20, 25, 30, 35, 40)` at codec
  adapter's mid-range preset (`"medium"` for libx264 / libx265 /
  libsvtav1). 5-point sweep is load-bearing default; do not widen
  it without ADR-0307 follow-up — Phase E callers downstream size
  their wall-time budget against five encodes per
  (resolution, target_vmaf) cell. Callers needing finer grid,
  Bayesian bisect, or precomputed corpus stream pass explicit
  `sampler=` — that seam stays open. Tests stub `iter_rows` via
  `monkeypatch.setattr(corpus_module, "iter_rows", ...)`; lazy
  `from .corpus import iter_rows` inside `_SamplerSettings.sample`
  (body of `_default_sampler`) resolves through patched module
  attribute on every call.
- **Default-sampler call order (HISS-04 split).** `_default_sampler`
  only bundles its kwargs into `_SamplerSettings` and runs
  `.sample()`; `make_default_sampler` returns
  `_SamplerSettings.bind()`, whose closure looks `_default_sampler`
  up at call time, so patching that attribute still reaches every
  bound sampler. Inside `sample`, `_default_sampler_preset(encoder)`
  runs before `_resolve_crf_sweep()` (unknown encoder `KeyError`
  wins over bad-sweep `ValueError`), then `_require_scorable_rows`
  -> `_capture_cloud` -> `pick_target_vmaf` (cloud sink gets every
  scored row before the per-cell collapse). Keep that order.
