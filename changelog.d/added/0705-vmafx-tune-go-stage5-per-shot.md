- `vmafx-tune-go tune-per-shot` — Go port of the Python `vmaf-tune
  tune-per-shot` per-shot CRF tuner (Stage 5 of the ADR-0705 staged migration,
  ADR-1124). Detects shot boundaries with `vmaf-perShot` (TransNet V2),
  applies the ADR-0513 uniform-time-window splitter, runs a CRF bisect against
  `--target-vmaf` inside each shot, and emits the FFmpeg encoding plan plus the
  concat-demuxer command. Plan JSON is byte-identical to the Python emitter —
  verified across all ten supported codecs — so existing consumers and the
  `report` ingester read Go output unchanged.
- New `pkg/pershot`: shot ranges, uniform-window splitter, `vmaf-perShot`
  JSON/CSV parsers, the `PredicateFn` tuning seam, plan construction, and a
  plan-JSON emitter that reproduces Python's `sort_keys` ordering, `repr()`
  float form, and `ensure_ascii` escaping.
- New `pkg/scorebackend`: libvmaf `--score-backend` resolution — `vmaf --help`
  parsing plus independent per-vendor hardware probes, honouring an explicit
  backend strictly rather than silently downgrading (ADR-0299 / ADR-0667).
- `pkg/encoder` gains the codec-adapter policy table (per-codec preset
  vocabulary, informative vs absolute quality windows, and the codec-correct
  `-c:v` argv shape required by HP-1 / ADR-0297 / ADR-0399), `AdapterEncoder`,
  an `InputArgs` field for pre-input ffmpeg options, and `ProbeSource` for the
  ffprobe geometry auto-probe (ADR-0548 / ADR-0509).
- `pkg/bisect` gains `YUVScoreFunc`, which decodes a containerised distorted
  file to raw YUV and invokes `vmaf` with full geometry, model and backend
  flags — the Y4M-only `VMAFScoreFunc` cannot score a raw-YUV reference.
- `--predicate-module` and `--fast-nr` have no Go implementation and now fail
  fast naming the Python fallback rather than being silently ignored: the
  first imports a Python callable at runtime (the Go seam is
  `pershot.PredicateFn`, library callers only), the second needs an
  `onnxruntime` binding. Both are tracked for Stage 6. Documented in
  `docs/usage/vmafx-tune-go.md`.
