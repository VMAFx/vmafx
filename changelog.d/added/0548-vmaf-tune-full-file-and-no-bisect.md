# vmaf-tune: tune-per-shot accepts container sources; compare --no-bisect mode

Two ergonomic gaps closed (ADR-0548):

**Fix A — tune-per-shot accepts container sources directly.**
`vmaf-tune tune-per-shot --src clip.mp4` now works without pre-extracting a raw
YUV or supplying `--width` / `--height` / `--framerate` manually. Geometry is
auto-derived from ffprobe for any non-YUV source (mp4, mkv, mov, ts, …). Raw YUV
sources still require explicit geometry. The probed values are written back onto
the `args` namespace so all downstream helpers (`_build_per_shot_bisect_predicate`,
`merge_shots`, plan serialisation) see consistent geometry without signature changes.

**Fix B — compare --no-bisect mode.**
`vmaf-tune compare --no-bisect --crf-sweep 18,23,28,33` encodes each (codec, CRF)
pair exactly once and reports (bitrate, VMAF) without running a target-VMAF bisect.
3 codecs × 4 CRFs = 12 rows in a single pass. Output is schema-version-3 JSON
(`"mode": "crf_sweep"`). `--target-vmaf` / `--target-vmafs` act as label-only knobs
(pareto frontier annotation) in this mode and do not drive the encode loop.
Hardware encoder unavailability produces `ok=false` rows per CRF rather than
aborting the run.
