- **`vmaf-tune compare` — codec-comparison mode (research-0061
  Bucket #7, ADR-0237 Phase A follow-up).** Given a single source and
  a target VMAF, `vmaf-tune compare --src REF.yuv --target-vmaf 92
  --encoders libx264,libx265,libsvtav1,libaom,libvvenc` runs each
  codec's recommend predicate in a thread pool and emits a ranked
  `(codec, best_crf, bitrate_kbps, encode_time_ms, vmaf_score)` table
  sorted by smallest file. Supports `--format markdown|json|csv` and
  `--output PATH`. Until Phase B's recommend backend lands, point
  `--predicate-module MODULE:CALLABLE` at any importable
  `(codec, src, target_vmaf) -> RecommendResult` callable to drive the
  ranking from a shim. Default `--encoders` resolves to every adapter
  currently registered in `codec_adapters/` — Phase A wires `libx264`
  only, so the canonical four / five codec invocation only ranks
  codecs whose adapters have already merged. New module
  `tools/vmaf-tune/src/vmaftune/compare.py` (predicate-driven
  orchestration + markdown / JSON / CSV renderers); 13 mocked smoke
  tests under `tools/vmaf-tune/tests/test_compare.py` (no `ffmpeg`,
  no built `vmaf` required). Schema exported as
  `vmaftune.compare.COMPARE_ROW_KEYS`. User docs:
  [`docs/usage/vmaf-tune.md`](../docs/usage/vmaf-tune.md) §"Codec
  comparison".
