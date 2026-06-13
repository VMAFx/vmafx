- **`tools/vmaf-tune/` Phase A — quality-aware encode automation scaffold
  (ADR-0237 Phase A Accepted, Research-0044).** New Python tool that
  drives FFmpeg over a `(preset, crf)` grid against `libx264`, scores
  each encode with the libvmaf CLI, and emits a JSONL corpus of
  `(source, encoder, params, bitrate, vmaf)` rows. Schema versioned via
  `vmaftune.SCHEMA_VERSION = 1` and exported as `CORPUS_ROW_KEYS`; the
  schema is the API contract that Phase B (target-VMAF bisect) and
  Phase C (per-title CRF predictor) will consume. Codec adapter
  registry (`codec_adapters/`) is multi-codec from day one — Phase A
  wires `libx264` only; subsequent codecs (`libx265`, `libsvtav1`,
  `libvpx-vp9`, `libvvenc`, neural extras) are one-file additions
  without touching the search loop. Subprocess-mocked smoke tests
  under `tools/vmaf-tune/tests/` (13 cases) cover command shape,
  version parsing, JSONL round-trip, encode-failure handling, and the
  schema contract — no `ffmpeg` or built `vmaf` binary required.
  User docs: [`docs/usage/vmaf-tune.md`](../docs/usage/vmaf-tune.md).
  Phases B–F remain Proposed under ADR-0237; this PR ships only the
  Phase A corpus scaffold.
