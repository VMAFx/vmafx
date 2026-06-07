# ADR-1077 — vmaf-tune corner cases: parse_versions + compare preset

| Field      | Value                             |
|------------|-----------------------------------|
| ADR number | 1077                              |
| Status     | Accepted                          |
| Date       | 2026-06-06                        |
| Author     | r14 parallel investigation agent  |

## Context

A systematic inspection of `tools/vmaf-tune` identified three latent defects in
two files. No crash was observed in CI (the affected code paths are only reached
when libaom-av1 or libvvenc are selected as encoders, or when `compare
--no-bisect --crf-sweep` is invoked without `--preset`), but all three bugs
would produce silently wrong output rather than an error.

### Bug A — `parse_versions` returns `"unknown"` for `libaom-av1` and `libvvenc`

`encode.py`'s `parse_versions` function has explicit branches for `libx264`,
`libx265`, `libsvtav1`, `libvpx-vp9`, and HW encoder tokens. `libaom-av1` and
`libvvenc` fall through to the `else` branch, which returns `"unknown"` (not
the stable adapter name, and not a parsed version). This causes corpus rows for
those two codecs to carry `"unknown"` as the encoder-version field, making
per-version conditioning in Phase B/C predictors impossible.

### Bug B — `_VERSION_PROBE_PATTERNS` missing `libaom-av1` and `libvvenc`

The fallback probe (`_probe_encoder_version_from_ffmpeg`) parses `ffmpeg
-version`'s configure-line for per-encoder enable flags. The dict only covered
`libx264`, `libsvtav1`, `libx265`, and `libvpx-vp9`. A libaom-av1 or libvvenc
probe therefore returned `""`, causing `probe_encoder_info` to report
`codec_detected=False` even for builds that include those codecs.

### Bug C — `compare --preset` default `None` silently fails all CRF-sweep rows

The `compare` subparser defined `--preset` with `default=None`. When
`compare --no-bisect --crf-sweep 20,28` is invoked without `--preset`, all
encodes produce `ok=False` rows because `bisect._encode_and_score` checks
`preset not in adapter.presets` — and `None not in <any tuple>` is always
`True`. The command exits 0, giving no indication that every encode was
rejected at the adapter gate.

## Decision

1. Add `libaom-av1` and `libvvenc` branches to `parse_versions` in
   `encode.py`. Both emit a version banner in some FFmpeg builds; where present
   the parsed version is returned (e.g. `"libaom-av1-3.6.0"`). Where the banner
   is absent the stable adapter name (`"libaom-av1"` / `"libvvenc"`) is
   returned rather than `"unknown"`, preserving a usable identifier for corpus
   conditioning.

2. Add `"libaom-av1"` and `"libvvenc"` entries to `_VERSION_PROBE_PATTERNS` so
   the `ffmpeg -version` configure-line probe (used by `probe_encoder_info` and
   the fallback path in `run_encode`) detects these codecs correctly.

3. Change the `compare --preset` argparse default from `None` to `"medium"`.
   `"medium"` is the canonical mid-preset for every registered adapter and
   matches the per-adapter `quality_default` preset. The `make_bisect_predicate`
   call and `_encode_and_score` both receive the now-non-None value, eliminating
   the silent-reject path.

## Consequences

- Corpus rows for `libaom-av1` and `libvvenc` now carry either a parsed version
  or the stable adapter name instead of `"unknown"`.
- `probe_encoder_info` correctly reports `codec_detected=True` for builds with
  `--enable-libaom` or `--enable-libvvenc`.
- `compare --no-bisect --crf-sweep` without `--preset` no longer silently
  produces all-failed rows.
- The existing test in `test_compare.py::test_cli_compare_binds_real_bisect_predicate`
  was updated to expect `"medium"` instead of `None`, matching the new default.

## Alternatives considered

**Return `"unknown"` as the libaom/vvenc fallback.** This was the pre-fix
behavior. It is strictly worse than returning the stable adapter name: nothing
downstream can distinguish "libaom-av1 build without banner output" from a
completely unknown encoder.

**Change `--preset` to required in `--no-bisect` mode.** This would make the
failure loud (argparse error) but would break `compare --no-bisect` invocations
that rely on per-adapter defaults. The `"medium"` default is the lowest-friction
fix and matches what every adapter treats as its canonical mid-point.

## References

- r14 parallel investigation: discovered via grep/read/test during
  `wf_bc4c515b-e0f-6` parallel audit.
- Related: ADR-0237 (codec adapter registry), ADR-0542 (CRF-sweep mode),
  ADR-0498 (probe-encoder-version follow-up chain).
