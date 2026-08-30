- **Nightly bisect tracker (issue #40) unsticks: `--check` parquet
  comparison now logical, sticky comment surfaces wiring breaks
  (ADR-0262).** The nightly `bisect-model-quality` workflow has
  red-lined every night since 2026-04-22 because the runner image
  upgraded `pyarrow` from 23.x to 24.x, which embeds
  `parquet-cpp-arrow version <X>.<Y>.<Z>` in the parquet header; the
  pre-existing `filecmp.cmp` byte equality on `features.parquet`
  treated that string as content drift. Worse, the workflow's
  sticky-comment-update step was gated on `result.json` existing,
  which `--check` failures never produce, so issue #40 silently
  froze on a 14-day-old success comment while the workflow ran red
  every night. Switches `ai/scripts/build_bisect_cache.py --check`
  parquet leg to `pyarrow.Table.equals` (schema + row count + values),
  ignoring writer metadata. ONNX byte-equality preserved
  (`producer_name` / `producer_version` / `ir_version` already
  pinned in `_save_linear_fr`). Adds `--wiring-broke` mode to
  `scripts/ci/post-bisect-comment.py` that posts a "WIRING BROKE"
  sticky-comment update with the cache-check stderr inline when
  `--check` itself fails, then exits non-zero so the run stays red.
  Documented in
  [ADR-0262](docs/adr/0262-bisect-cache-logical-comparison.md);
  relaxes ADR-0109 §Decision (parquet only).
