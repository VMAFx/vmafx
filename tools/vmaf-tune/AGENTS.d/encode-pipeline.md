---
paths:
  - tools/vmaf-tune/src/vmaftune/encode.py
  - tools/vmaf-tune/src/vmaftune/encoder_stats.py
  - tools/vmaf-tune/tests/test_encode*.py
invariant: Subprocess boundary is test seam; run_encode_with_stats captures encoder internals; -f rawvideo before seek.
---
<!-- markdownlint-disable MD024 -->
# Subprocess encode pipeline and stats capture

- **Subprocess boundary is test seam.** `encode.run_encode` and
  `score.run_score` accept `runner` argument that defaults to
  `subprocess.run`. Tests inject fake; production callers leave it
  default. Do not reach for `os.system` / `popen` shortcuts —
  `tests/test_corpus.py` will silently stop covering path.

- **Encode pipeline (`encode.py`) is still x264-CRF-tied.**
  ADR-0281 added QSV adapter classes but did not widen
  `build_ffmpeg_command` to dispatch on `adapter.quality_knob`.
  Until that follow-up lands, QSV adapters validate
  `(preset, global_quality)` correctly but harness will not yet
  successfully drive QSV encode end-to-end.
- **Subprocess boundary is test seam.** `encode.run_encode`,
  `score.run_score`, and QSV `ffmpeg_supports_encoder` probe
  accept `runner` argument that defaults to `subprocess.run`.
  Tests inject fake; production callers leave it default. Do not
  reach for `os.system` / `popen` shortcuts —
  `tests/test_corpus.py` and `tests/test_codec_adapter_qsv.py`
  will silently stop covering path.

## ADR-0332 invariants (encoder-internal stats capture)

- Corpus row schema is at v3; new columns added to
  ``CORPUS_ROW_KEYS`` and ``SCHEMA_VERSION`` must keep v3 ten
  ``enc_internal_*`` columns positionally stable so v2 readers see
  zero rather than missing key. Coordinates with ADR-0302.
- Every adapter in ``codec_adapters/`` must declare
  ``supports_encoder_stats: bool`` (no Protocol default). x264 /
  x265 set True; everything else False until codec-specific
  parser lands. x265's ``q-aq`` and ``icu`` / ``pcu`` / ``scu``
  pass-1 aliases are intentionally normalised in
  ``encoder_stats.py`` so corpus rows keep same ten
  ``enc_internal_*`` columns as x264.
- ``run_encode_with_stats`` doubles per-encode wall-clock on
  opt-in adapters by design. Do not collapse pass-1 + pass-2 calls
  into one — encoder won't emit parseable stats file outside
  ``-pass 1`` mode.

- **Pass-1 input args are not `_build_input_args` (HISS-04 split).**
  `build_pass1_stats_command` uses `_pass1_input_args`. It shares
  `_raw_demuxer_args` with `_build_input_args` but keeps its own clip
  guard: the `duration_s` fallback requires
  `sample_clip_seconds <= 0.0`, so a NaN clip length emits no `-t`
  there, while `_build_seek_args` does emit one. Merging the two
  changes pass-1 argv; do it only as a tested behaviour change.
- **`_build_input_args` emits `-f rawvideo` block before seek args**
  (`encode.py`). `-ss` / `-t` must stay input-side, ahead of `-i`, or
  ffmpeg decodes whole source. ADR-0506 / Bug #V6-1.
