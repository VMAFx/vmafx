<!-- markdownlint-disable MD013 MD060 -->
# ADR-0548: vmaf-tune tune-per-shot accepts container sources directly; compare gains --no-bisect mode

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vmaf-tune`, `cli`, `ergonomics`, `compare`, `per-shot`

## Context

Two ergonomic gaps in `vmaf-tune` require separate raw-YUV pre-processing steps before
the tool's core workflows can begin:

**Gap 1 — tune-per-shot requires a pre-extracted YUV.**
The `tune-per-shot` subcommand accepted only raw `.yuv` sources and required the operator
to supply `--width`, `--height`, `--framerate`, and `--total-frames`. For a 10-minute 4K
container (mp4/mkv) that meant a 30 GB intermediate file taking over a minute to write
just to start shot detection. The operator had to know the geometry in advance or run
`ffprobe` manually.

**Gap 2 — compare always bisects.**
The `compare` subcommand always runs a target-VMAF bisect per (codec, target) cell.
Operators who want to sweep a fixed CRF ladder (e.g. 18, 23, 28, 33) and report the
resulting (bitrate, VMAF) pairs had no direct path — they had to either run the corpus
subcommand and post-process, or script multiple single-target bisect calls.

Both gaps were reported by the user as blocking the "sweep the codec ladder + report"
and "full-file per-shot tune from an mp4" workflows.

## Decision

**Fix A**: Make `--width`, `--height`, and `--framerate` optional for `tune-per-shot`.
When `--src` is a container file (detected by suffix — anything except `.yuv` / `.raw`),
auto-probe geometry via the existing `vmaftune.report.probe_source` helper (ffprobe
wrapper). `--total-frames` is also derived from the probe. Raw YUV sources still require
explicit geometry. The probed values are written back to the `args` namespace so all
downstream helpers (`_build_per_shot_bisect_predicate`, `merge_shots`, plan serialisation)
see consistent geometry without any signature changes.

**Fix B**: Add `--no-bisect` and `--crf-sweep` flags to `compare`. When `--no-bisect` is
set, the command encodes each (codec, CRF) pair from `--crf-sweep` using
`bisect._encode_and_score` directly (one real encode+score per cell, no iterative search).
Results are emitted as schema-version-3 JSON with a `rows` list — one entry per
(codec, crf) — and a `mode: "crf_sweep"` discriminator. `--target-vmaf` / `--target-vmafs`
become label-only knobs in this mode (they annotate the pareto frontier markers in the
rendered report but do not drive the encode loop). Hardware encoder availability is probed
the same way as the bisect path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Write a 30 GB temp YUV for Fix A** | No code change | 1+ min write for 4K 10-min source; disk space waste | The whole point is to avoid the intermediate file |
| **Pipe-decode to vmaf-perShot stdin for Fix A** | No temp files at all | vmaf-perShot does not yet support stdin as a video source (it calls the C rawvideo API which needs seekable input) | Requires binary change outside this PR's scope; temp-YUV approach in `_extract_shot_to_raw_yuv` is already per-shot so each temp file is seconds long, not the full source |
| **New `sweep` subcommand for Fix B** | Clean separation from compare | Separate subcommand surface to document and learn | The CRF-sweep is logically "compare without bisect"; reusing the compare subcommand with a flag preserves the familiar surface and shares encoder-availability probing |
| **Reuse the corpus JSONL pipeline for Fix B** | Already exists | Requires a Phase A corpus run first; no direct JSON-with-rows output | Too many steps for a simple "encode these CRFs and report" use case |

## Consequences

- **Positive**: operators can run `vmaf-tune tune-per-shot --src clip.mp4 ...` without
  pre-extracting YUV or knowing geometry. The 30 GB intermediate file is replaced by
  per-shot temp YUVs (seconds long each) that are cleaned up automatically.
- **Positive**: `vmaf-tune compare --no-bisect --crf-sweep 18,23,28,33` produces a
  12-row JSON (3 codecs × 4 CRFs) in one pass — much faster than 4 bisect sweeps for
  operators who want a fixed ladder rather than a target-VMAF search.
- **Negative**: the auto-probe adds one `ffprobe` subprocess per `tune-per-shot`
  invocation on container sources. Cost is negligible (< 1 s) relative to shot
  detection and per-shot encoding.
- **Neutral**: `--width` and `--height` remain required for raw YUV sources; no
  existing scripts break. The `--no-bisect` flag is ignored when not passed (opt-in).
- **Neutral / follow-up**: the `--format` flag for `compare --no-bisect` currently only
  emits JSON (the mode routes around the standard emitters). Markdown / CSV rendering
  of the schema-v3 payload is a follow-up if demanded.

## References

- `vmaftune.report.probe_source` — the existing ffprobe wrapper reused for Fix A.
- `vmaftune.bisect._encode_and_score` — the encode+score primitive reused for Fix B.
- `vmaftune.compare.probe_encoder_available` — hardware-encoder probe reused for Fix B.
- Related PR: this feature PR (feat/vmaf-tune-full-file-and-no-bisect).
- req: "tune-per-shot accepts mp4/mkv source directly (not just raw YUV)" and
  "compare gains a --no-bisect mode" (paraphrased from user request 2026-05-18).
