<!-- markdownlint-disable MD018 -->
# ADR-0392: `vmaf-tune` Phase D — per-shot CRF tuning

- **Status**: Accepted (CLI bisect wiring landed 2026-05-14; native
  per-codec emission still deferred)
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ai, ffmpeg, codec, automation, fork-local

## Context

Phase A of `vmaf-tune` (corpus tooling, [ADR-0237](0237-quality-aware-encode-automation.md),
PR #329) shipped the grid-sweep harness. Research-0061 — the
`vmaf-tune` capability audit — ranked **per-shot CRF tuning** (Bucket
#1) as the table-stakes Netflix-equivalent feature: the canonical
2018 paper reports 10–30 % bitrate savings at constant VMAF when CRF
is varied per shot instead of held flat across the title.

The fork already ships every component the orchestration needs:

- **TransNet V2** real-weights ONNX model at
  `model/tiny/transnet_v2.onnx` ([ADR-0223](0223-transnet-v2-shot-detector.md)),
  consumed by the C-side `vmaf-perShot` CLI ([ADR-0222](0222-vmaf-per-shot-tool.md)).
- **Phase A corpus + codec adapter contract** at
  `tools/vmaf-tune/src/vmaftune/`.
- **Phase B target-VMAF bisect** — its predicate shape
  ``(shot, target_vmaf, encoder) -> (crf, measured_or_predicted_vmaf)``
  is the hook the per-shot loop calls per shot.

What is missing is the **orchestration layer** that ties shot
detection to per-shot CRF selection to FFmpeg encode + concat. This
ADR originally shipped that layer as a scaffold with stable public API,
two explicitly-pluggable integration seams, and mocked smoke coverage.

The scaffold is gated by three follow-up dependencies we deliberately
do not ship in the same PR:

1. Per-codec native per-shot emission (x264 `--qpfile`, x265
   `--zones`, SVT-AV1 segment tables) must land per-codec — the
   default path uses per-segment encode plus concat-demuxer, which is
   portable but loses GOP-alignment efficiency.
2. A held-out per-shot validation corpus must exist so we can claim
   numerical wins; without it, this PR ships zero quality claims.

Status update 2026-05-14: Phase B bisect has landed and the
`vmaf-tune tune-per-shot` CLI now binds the predicate seam to
`bisect_target_vmaf` by default. The CLI extracts each detected
half-open shot to a temporary raw-YUV reference, runs the real
encode+score bisect for that shot, and records the measured VMAF in
the JSON plan. `--predicate-module MODULE:CALLABLE` remains the
explicit custom/test hook; the library-only default predicate still
returns the adapter default CRF for dry-run callers that invoke
`tune_per_shot()` without geometry.

## Decision

We will ship `tools/vmaf-tune/src/vmaftune/per_shot.py` as the
Phase D orchestration layer with the following public surface:

- ``Shot(start_frame, end_frame)`` — half-open frame range. The
  ``vmaf-perShot`` CSV/JSON sidecar uses inclusive ``end_frame``; we
  normalise into the half-open form at the parse boundary.
- ``ShotRecommendation(shot, crf, predicted_vmaf)``.
- ``EncodingPlan(recommendations, encoder, framerate, segment_commands,
  concat_command, concat_listing)`` — segments + the FFmpeg argv to
  realise them.
- ``detect_shots(video_path, *, per_shot_bin="vmaf-perShot", runner=...)``
  — calls the C-side binary; falls back to a single-shot range if the
  binary is missing or fails. ``runner`` is the test seam.
- ``tune_per_shot(shots, *, target_vmaf, encoder, predicate=None)``
  — drives the predicate per shot. CLI callers get the Phase-B
  bisect predicate by default; library callers that omit a predicate
  get the codec adapter's ``quality_default`` for deterministic dry
  runs.
- ``merge_shots(recs, *, source, output, framerate, encoder,
  segment_dir=..., ffmpeg_bin=...)`` — emits one ``ffmpeg`` argv
  per shot (using ``-ss`` + ``-frames:v``) plus a final concat-demuxer
  command.

The CLI subcommand is ``vmaf-tune tune-per-shot``. JSON plan to
stdout by default, ``--plan-out`` and ``--script-out`` for files.
Default CLI behaviour is real per-shot bisect unless
``--predicate-module`` is supplied.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Pluggable scaffold (chosen)** | Stable public API; tests run with mocks; Phase B + per-codec emission land as drop-ins | Ships zero quality wins until follow-ups land; risk of stalling at "scaffold" status | Picked: matches Phase A's deliberately-scaffolded pattern; the alternative (gate Phase D on every dependency) blocks the audit's Bucket #1 indefinitely |
| Wait for Phase B + per-codec emission first | Single PR ships the full feature with measurable savings | Bundles three independent workstreams into one giant PR; gates each on the slowest; impossible to review incrementally | Rejected: the integration layer is independently valuable as a forcing function; landing it first surfaces interface mismatches in Phase B / codec adapters |
| Native ``--qpfile`` / ``--zones`` per codec from day one | Keeps the encoder's GOP and rate-control coherent across shot boundaries | Requires every codec adapter to grow per-shot emission *before* the orchestrator exists; couples Phase D's PR to the codec adapter cadence | Rejected: per-segment + concat is portable across every codec; native emission is a per-adapter optimisation that lands in the codec PRs |
| Inline shot detection (call ONNX Runtime directly from Python) | One fewer subprocess; tighter integration | Duplicates ``vmaf-perShot``'s detection logic; bypasses the C-side preprocessing the perShot binary already does (luma planes, 27×48 thumbnails) | Rejected: ``vmaf-perShot`` is the canonical detector; reusing it preserves a single point of truth for shot boundaries across the fork |
| Phase B bisect inlined into Phase D | Single PR ships the bisect + the per-shot loop together | Couples two independently-tested workstreams; doubles the test surface; Phase B already has its own PR (#347) | Rejected: keep the bisect surface in its own ADR / PR; the predicate seam is the contract between them |
| **CLI binds existing Phase B bisect (2026-05-14 choice)** | Retires the CLI scaffold without changing the library predicate API; reuses tested bisect monotonicity and adapter validation | Extracts each shot to temporary raw YUV before bisect, so long clips pay extra disk I/O | Picked: smallest real implementation now that Phase B exists; native per-codec zones remain a separate optimization |
| Make `tune_per_shot()` require a predicate | Prevents accidental adapter-default dry runs | Breaks existing programmatic smoke users and tests that intentionally exercise the dry-run API | Rejected: CLI is the user-discoverable production path; the library dry-run fallback is explicitly documented as non-production |

## Consequences

- **Positive**:
  - Closes the audit's Bucket #1 scope at the orchestration layer.
  - Stable public API (``detect_shots`` / ``tune_per_shot`` /
    ``merge_shots``) that Phase B and per-codec emission can plug
    into without breaking callers.
  - Mocked smoke coverage: tests pass without ``ffmpeg``,
    ``vmaf``, or ``vmaf-perShot`` on PATH.
  - First end-to-end downstream consumer of TransNet V2's real
    weights — exercises the ``vmaf-perShot`` binary surface.

- **Negative**:
  - Per-segment + concat encoding loses keyframe alignment with the
    encoder's natural GOP — efficiency penalty vs native
    ``--qpfile``/``--zones``. Acceptable for the portable path; the
    codec PRs replace it per-codec.
  - The bisect path extracts each shot to raw YUV first; operators
    should expect temporary disk usage proportional to the largest
    in-flight shot.
  - Two-language surface (Python orchestration calling a C-side
    binary) means a `vmaf-perShot` regression silently degrades to
    the single-shot fallback. Logged via a follow-up: emit a stderr
    warning when fallback fires in production runs.

- **Neutral / follow-ups**:
  - Per-codec PRs (x265, SVT-AV1, libaom, libvvenc) extend the
    codec adapter contract with an ``emit_per_shot_overrides`` hook
    (already declared in ADR-0237) that ``merge_shots`` will dispatch
    to instead of the per-segment + concat fallback.
  - Held-out per-shot validation corpus is a separate research item;
    likely BVI-DVC + Netflix Public + KoNViD subsets re-encoded
    through Phase A's grid sweep.
  - MCP integration (Phase F) gains a ``per_shot_plan`` tool once
    this scaffold lands.

## References

- Source: `req` 2026-05-03 — "Scaffold Phase D of `vmaf-tune` per
  PR #354's audit (Bucket #1, M effort, 'Netflix per-shot
  table-stakes')." Don't fully implement — ship a working scaffold
  design ADR.
- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune`
  umbrella; this ADR is its first per-phase split.
- [ADR-0222](0222-vmaf-per-shot-tool.md) — C-side `vmaf-perShot` CLI
  surface consumed by ``detect_shots``.
- [ADR-0223](0223-transnet-v2-shot-detector.md) — TransNet V2 real
  weights driving the shot detector.
- [Research-0044](../research/0044-quality-aware-encode-automation.md)
  — option-space digest covering encode-search strategies.
- Research-0061 (PR #354) — `vmaf-tune` capability audit, Bucket #1
  ranks per-shot tuning as the M-effort, High-impact next step.
- Netflix tech blog 2018 — *Per-Title Encode Optimization* and the
  follow-on per-shot dynamic optimiser; the public 10–30 % bitrate
  savings figure motivating Bucket #1.
