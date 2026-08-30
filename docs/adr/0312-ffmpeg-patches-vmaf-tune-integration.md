<!-- markdownlint-disable MD013 MD060 -->
# ADR-0312: FFmpeg-patch series for vmaf-tune integration (qpfile + libvmaf_tune + pass-autotune)

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, vmaf-tune, patch-series, scaffold

## Context

The fork ships its FFmpeg integration as a stack of patches against
`n8.1` under [`ffmpeg-patches/`](../../ffmpeg-patches/). Patches
`0001`–`0006` wire libvmaf-side surfaces (tiny model, vmaf_pre,
SYCL/Vulkan backends and zero-copy filters). **None** of them wire
`vmaf-tune` — the Python encode-tuning orchestrator under
[`tools/vmaf-tune/`](../../tools/vmaf-tune/) — into FFmpeg.

vmaf-tune today drives encodes by shelling out to `ffmpeg` with
encoder-specific knobs. The `saliency.py` module in particular emits
a per-MB QP-offset map that's only consumable by libx264 (`-x264-params
qpfile=…`). SVT-AV1 and libaom-av1 have their own ROI-input dialects
(`--region-of-interest-csv` and `AV1E_SET_ROI_MAP` respectively).
Without a unified `-qpfile` plumbing, the saliency feature stays
single-encoder.

Two further surfaces would benefit from a thin FFmpeg-side hook:

1. An in-process VMAF scorer that runs alongside a 1-pass encode and
   suggests a CRF for the next pass — today the recommend loop in
   `tools/vmaf-tune/src/vmaftune/recommend.py` re-encodes-and-scores
   externally via subprocess.
2. A `-pass autotune` value on FFmpeg's `-pass` argument that signals
   "this is a vmaf-tune-orchestrated 2-pass encode" and emits a
   pointer to the user-facing docs.

## Decision

Ship three patches as the next contiguous run of the `ffmpeg-patches/`
series (0007 / 0008 / 0009):

1. **Patch 0007** — `-qpfile <path>` AVOption on `libx264`,
   `libsvtav1`, and `libaom-av1`. A new shared parser at
   `libavcodec/qpfile_parser.{c,h}` reads the vmaf-tune qpfile format
   once. libx264 forwards the path to x264's native per-MB qpfile
   reader (supported since r2390). SVT-AV1 and libaom were **scaffold**
   at the time of this ADR (parse, validate, log; full ROI-bridge
   wiring deferred). Both deferrals have since been retired:
   SVT-AV1 ROI bridge via PR #417 (per-frame `ROI_MAP_EVENT` priv-data
   enable_roi_map`, gated on SVT-AV1 1.6.0+); libaom-av1 ROI bridge
   via PR #419 (`AOME_SET_ROI_MAP` per frame with 8-segment uniform
   binning of qp_offsets). Patch 0007 is now full-impl across all
   three encoders.
2. **Patch 0008** — new `libvmaf_tune` 2-input video filter
   (`libavfilter/vf_libvmaf_tune.c`). **Scaffold**: filter skeleton,
   AVOption table, init/uninit, frame pass-through with framesync,
   final-line `recommended_crf=…` log line at uninit. Recommend logic
   is a thin linear CRF↔VMAF interpolation; the full Optuna TPE loop
   stays in vmaf-tune.
3. **Patch 0009** — `-pass-autotune` advisory flag on `fftools/ffmpeg_opt.c`.
   Emits a stderr log pointing at `docs/usage/vmaf-tune-ffmpeg.md`.
   Glue only.

The new-file headers carry the fork copyright
(`Copyright 2026 Lusoris and Claude (Anthropic)`); modifications to
upstream-mirrored files (`libx264.c`, `libsvtav1.c`, `libaomenc.c`,
`ffmpeg_opt.c`, `Makefile`, `allfilters.c`, `configure`) keep their
existing FFmpeg/LGPL headers.

## Alternatives considered

| Option                                         | Pros                                                          | Cons                                                                                              |
|------------------------------------------------|---------------------------------------------------------------|---------------------------------------------------------------------------------------------------|
| **Ship as 3-patch series** (chosen)            | Each patch is reviewable on its own; bisectable; mirrors the upstream FFmpeg patch-submission rhythm. | Three patches to keep aligned with each other and with `tools/vmaf-tune/`. |
| One mega-patch                                 | Fewer files, single review.                                   | Unbisectable; reviewer has to follow three independent feature surfaces in one diff; rebase risk multiplies. |
| Defer until vmaf-tune stabilises               | Less churn while vmaf-tune iterates.                          | The qpfile bridge is already blocking saliency on AV1 encoders; users keep hitting the wall.       |
| Overload `-pass <int>` with a string value     | Reuses existing flag.                                         | `-pass` is `OPT_TYPE_INT` in n8.1; string-overload requires changing the option type — invasive and not reversible from a patch series. |
| Full ROI-bridge for SVT-AV1 / libaom in 0007   | End-to-end behaviour from day one.                            | Each encoder's ROI ABI is non-trivial (SVT-AV1 CSV format, libaom 4-segment quantizer map per frame); doubling the patch's LOC budget against an already-large series. |
| Full libvmaf scoring in 0008                   | Real recommended-CRF values immediately.                      | Re-implements the framesync + scoring pipeline already in `vf_libvmaf.c`; better as a dedicated follow-up patch when the scoring strategy is settled. |

## Consequences

### Positive

- vmaf-tune's saliency path is encoder-agnostic at the CLI level: the
  `-qpfile` flag works the same way across libx264, libsvtav1, and
  libaom-av1, with full ROI-bridge behaviour on each (PRs #409 + #417 +
  #419 closed all three encoder branches end-to-end).
- The shared parser at `libavcodec/qpfile_parser.{c,h}` is a single
  point of evolution when the qpfile format grows new columns.
- The `libvmaf_tune` filter and `-pass-autotune` flag give vmaf-tune
  a stable FFmpeg-side ABI to grow into.

### Negative

- Patches 0008 (`vf_libvmaf_tune` filter) and 0009 (`-pass-autotune`
  CLI glue) remain scaffold by design. Patch 0008's recommend logic is
  a linear CRF↔VMAF interpolation; the Optuna TPE loop stays in
  vmaf-tune. Patch 0009 emits a stderr advisory only. Both are
  reviewable seams for future expansion. *(Status update 2026-05-06:
  patch 0007's two original scaffold hunks are now full-impl per the
  decision-section update above. Patches 0008/0009 stay scaffold; that
  was always the intent and is not tracked as deferred work.)*
- Patches 0007–0009 must be kept consistent with vmaf-tune's
  `saliency.py` qpfile format — see CLAUDE.md §12 r14 and the
  vmaf-tune patch invariant in `ffmpeg-patches/README.md`.

### Verification

`make lint` clean on the fork-side artefacts (parser is C99, bounded
loops, no banned APIs). The series-replay command in CLAUDE.md §12
r14 applies all 9 patches cleanly to pristine `n8.1`. libx264 +
libsvtav1 + libaomenc + qpfile_parser objects compile against
FFmpeg n8.1 (smoke build verified — see PR description). libx264's
`-qpfile` option is recognised by ffmpeg's argument parser
(`-qpfile /nonexistent` produces the new error message instead of
"unrecognized option").

## References

- req: "we don't have an ffmpeg patch for vmaf-tune yet?" (user prompt
  2026-05-05) — confirmed scope: three patches (qpfile unification,
  libvmaf_tune filter scaffold, pass-autotune CLI glue).
- [docs/research/0084-ffmpeg-patch-vmaf-tune-integration-survey.md](../research/0084-ffmpeg-patch-vmaf-tune-integration-survey.md)
- [tools/vmaf-tune/src/vmaftune/saliency.py](../../tools/vmaf-tune/src/vmaftune/saliency.py)
- [tools/vmaf-tune/src/vmaftune/recommend.py](../../tools/vmaf-tune/src/vmaftune/recommend.py)
- [ADR-0247: vmaf-roi sidecar](./0247-vmaf-roi-c-sidecar.md) — sibling roi-map work
- [ADR-0237: vmaf-tune harness](./0237-vmaf-tune-harness.md) — parent feature
- [ADR-0286: saliency_student_v1](./0286-saliency-student-model-v1.md) — model that emits the qpfile
- [CLAUDE.md §12 r14](../../CLAUDE.md) — patches-must-update-with-libvmaf rule
