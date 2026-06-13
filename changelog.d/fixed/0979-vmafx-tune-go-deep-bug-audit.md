- `vmafx-tune-go` deep bug audit (5 fixes):
  - **JSON NaN propagation in `bisect_samples`**: `pkg/report.EmitJSON`
    and `cmd/vmafx-tune/cmd.emitSweepJSON` previously sanitised
    only the top-level row floats, leaving `bisect.Sample` fields
    (`bitrate_kbps`, `vmaf_score`, `encode_time_ms`) as raw `float64`
    in the JSON wire shape. A single non-finite value (e.g. from a
    corrupt vmaf XML mean) crashed `json.MarshalIndent` with
    `"unsupported value: NaN"` and broke the Python ↔ Go parser-parity
    invariant (AGENTS.md #2). Introduces `report.SanitizeBisectSamples`
    which coerces nested floats to JSON `null`; mirrored in the
    ladder emitter (Cloud + Hull + Renditions all sanitised across
    `BitratekBps`, `VMAF`, `TargetVMAF`).
  - **`parseVMAFXMLMean` accepts the literal "NaN" / "±Inf"**:
    `strconv.ParseFloat` returns NaN without error for those tokens.
    The parser now rejects non-finite means at the source so the bisect
    step records a score failure rather than propagating a corrupt
    score into the rest of the pipeline.
  - **Subprocess hang risk (`ffmpeg`, `vmaf`, `ffprobe`)**: every
    `exec.Command` in `pkg/encoder` and `pkg/bisect` is now
    `exec.CommandContext` with a per-call timeout. Defaults:
    `60m` ffmpeg encode, `30m` vmaf score, `30s` ffprobe bitrate
    probe, `30s` codec discovery. Overridable via
    `VMAFX_TUNE_ENCODE_TIMEOUT`, `VMAFX_TUNE_SCORE_TIMEOUT`,
    `VMAFX_TUNE_PROBE_TIMEOUT`.
  - **Codec discovery cache silently returned stale results on
    binary-path change**: the previous `sync.Once`-based cache
    locked in whichever ffmpeg path was probed first, with a
    `_ = ffmpegBin` no-op pretending to invalidate the cache. After
    the fix the cache key is the binary path; calling with a
    different path triggers a re-probe and replaces the cache.
    `RefreshCodecCache` now updates the cache key alongside the map.
