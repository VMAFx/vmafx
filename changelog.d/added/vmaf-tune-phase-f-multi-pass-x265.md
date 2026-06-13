- **`vmaf-tune --two-pass` — Phase F multi-pass encoding seam, libx265
  first ([ADR-0333](../docs/adr/0333-vmaf-tune-multi-pass-encoding.md)).**
  Codec adapters opting in declare `supports_two_pass = True` and
  override `two_pass_args(pass_number, stats_path) -> tuple[str, ...]`;
  `X265Adapter` is the first concrete implementation, returning
  `('-x265-params', f'pass={N}:stats={path}')`. `EncodeRequest` gains
  `pass_number: int = 0` (0 = single-pass / default; 1 / 2 = pass index)
  and `stats_path: Path | None = None`; `build_ffmpeg_command` redirects
  pass-1 output to `-f null -` so the throwaway encoded bitstream isn't
  written. New `encode.run_two_pass_encode(req, ...)` materialises a
  per-encode unique stats file under a tempdir, runs pass 1 → pass 2,
  cleans up the stats file (and libx265's `.cutree` sidecar) on exit,
  and returns one combined `EncodeResult` (encode_time = sum of both
  passes; size = pass-2 size). New `--two-pass` CLI flag opts in on
  `corpus` / `recommend`; default stays single-pass. Codecs where
  `supports_two_pass = False` fall back to single-pass with a stderr
  warning (matches the saliency.py x264-only fallback precedent);
  callers using the Python API can pass `on_unsupported="raise"` to
  fail loudly instead. Sibling codec adapters (libx264, libsvtav1,
  libvvenc, libaom-av1) inherit the seam and land as one-file follow-up
  PRs. NVENC's `-multipass` is a separate adapter contract (single-
  invocation lookahead, not the stats-file two-call sequence) and is
  not covered by this seam. AMF / QSV / VideoToolbox keep
  `supports_two_pass = False` (hardware encoders use internal
  lookahead).
