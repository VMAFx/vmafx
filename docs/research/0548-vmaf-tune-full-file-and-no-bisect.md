# Research digest 0548 — vmaf-tune ergonomics: container sources + CRF sweep mode

**Status:** Trivial ergonomic fix — no digest needed beyond this summary.
**ADR:** [ADR-0548](../adr/0548-vmaf-tune-full-file-and-no-bisect.md)
**Date:** 2026-05-18

## Summary

Two operator-reported workflow blockers with a clear implementation path each.
No novel algorithm research required; both fixes reuse existing in-tree primitives.

### Fix A: tune-per-shot container source auto-probe

The existing `vmaftune.report.probe_source` ffprobe wrapper (introduced in
ADR-0509 for the `compare` subcommand) already returns `SourceInfo` with
`width`, `height`, `fps`, `duration_s`, and `frame_count`. The only work
was calling it from `_run_tune_per_shot` when `--src` is not a raw YUV and
writing the result back onto the `args` namespace before any geometry-consuming
helper runs. No new subprocess, no new dependency.

**Why no temp-YUV approach:** The `_extract_shot_to_raw_yuv` helper inside
`tune-per-shot` already extracts per-shot clips as temporary YUV files (each
a few seconds long) and cleans them up after encoding. The 30 GB full-source
temp file approach was never necessary — only the geometry was missing.

### Fix B: compare --no-bisect CRF sweep

The existing `bisect._encode_and_score` primitive already handles a single
encode+score call without any iterative loop. Running it once per (codec, CRF)
cell and collecting results into a list produces exactly the fixed-ladder output
operators requested. The existing `ThreadPoolExecutor` pattern from
`compare_codecs_sweep` provides parallelism for free.

Schema-version-3 was chosen rather than reusing v2 because the discriminating
field (`mode: "crf_sweep"` vs `target_vmafs` list) is structurally different
enough that a separate version avoids ambiguous parser branches in downstream
consumers (`vmaf-tune report`, external scripts).

## Alternatives not chosen

See ADR-0542 `## Alternatives considered` table.
