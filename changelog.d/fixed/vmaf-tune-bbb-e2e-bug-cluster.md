- **vmaf-tune**: Fix the 2026-05-17 BBB end-to-end bug cluster across
  `compare` / `ladder` / `tune-per-shot` / `report` (ADR-0497):
  - `bisect` now decodes the encoded `.mkv` artefact to raw YUV before
    invoking the libvmaf CLI (previously aborted with "file too small
    for declared geometry"). The shared helper `score.maybe_decode_distorted`
    centralises the decode path.
  - `bisect` / `compare` autodetect container `--src` (`.mp4` / `.mkv`)
    and propagate `source_is_container=True` so the encoder ffmpeg
    invocation skips the `-f rawvideo` input flags.
  - `compare --format json` emits `null` for non-finite numerics
    (RFC 8259-strict); strict parsers (Go, Rust, `jq --strict`) load
    the output cleanly. Failed rows stay distinguishable via the
    per-row `ok` flag.
  - `ladder` exposes `--framerate` / `--duration` / `--pix-fmt`
    source-shape flags (symmetric with `compare` / `tune-per-shot`)
    plus `--crf-sweep` for smoke runs that want a shorter sweep than
    the canonical `(18,23,28,33,38)` grid.
  - `report` renders `NaN` / missing cells as em-dash (`—`) instead
    of `nan kbps`; aggregates the top-level `ok` flag from row-level
    flags (was unconditionally `true`); adds `codec_rows_ok` /
    `codec_rows_failed` counts to the stdout summary.
