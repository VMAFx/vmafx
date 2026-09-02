# ADR-0505: vmaf-tune ladder container-source encode + full per-CRF sample cloud

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: vmaf-tune, ladder, corpus, encode, vmaf-cli, docs

## Context

The BBB end-to-end v5 probe (after PR #1258 / ADR-0501 closed the
v4 cluster) surfaced three follow-ups that fall into two root
causes:

- **V5-1** — `vmaf --backend vulkan` strict-mode refusal was again
  reported as exiting `0` on the dev-mcp container. A live re-run
  against the post-ADR-0501 binary shows the propagation chain
  `init_gpu_backends() -> ret = -1 -> main() -> exit(255)` is
  intact, so the *behaviour* is correct — but the V4-A integration
  test that pinned it was gated on `shutil.which("vmaf")`. On every
  developer host that did not install the binary onto `$PATH`
  (including the dev-mcp container, where the binary lives at
  `/usr/local/bin/vmaf` but isn't reached through a relative
  invocation in the test's pytest harness) the gate silently
  disengaged. A future RC=0 regression would have shipped without
  the regression test firing.
- **V5-2** — `vmaf-tune ladder` against a container source
  (`bbb_sunflower_1080p_30fps_normal.mp4`) produced VMAF in the
  4-9 band at ~50 Mbps regardless of CRF — a uniformly-bogus
  encode. Root cause: `corpus.iter_rows` instantiates
  `EncodeRequest` without ever inspecting whether the source is a
  container; the encode argv builder (`encode.build_ffmpeg_command`)
  therefore emits the raw-video framing flags
  (`-f rawvideo -pix_fmt yuv420p -s WxH -r FR -i src.mp4`) against
  every source. FFmpeg reads the MP4's compressed payload as planar
  YUV pixels — the resulting "encode" is bogus regardless of CRF,
  and the per-CRF cloud is uniformly low quality. ADR-0501's
  reference-side scale fix corrected the score-side geometry but
  did nothing for the encode-side container detection.
- **V5-3** — The `samples[]` array in the v1 JSON descriptor
  double-listed every rendition. Root cause: ADR-0501's emit path
  derived the sample cloud from `ladder.points`, which contains
  one row per `(resolution, target_vmaf)` cell. The CLI ships
  `--crf-sweep` so the sampler scores a multi-CRF grid per cell,
  but only the `pick_target_vmaf` winner survived into
  `ladder.points`. Two target-VMAFs that converged on the same
  CRF emitted two identical sample rows; every non-winning CRF
  was silently dropped (the V5-2 symptom "only top-CRF kept per
  resolution").

The V5-2 garbage encode is the dominant failure — even with the
V5-3 sample cloud fixed, the VMAF would still read 4-9 against
the container source. The V5-1 test gate is independent.

## Decision

1. **V5-1 test hardening**: rewrite the regression test under
   `tools/vmaf-tune/tests/test_bbb_e2e_v5_bug_cluster.py` to probe
   three reachable locations for the `vmaf` binary in order:
   `$VMAF_BIN_FOR_TESTS` env override, `shutil.which("vmaf")`,
   then the canonical meson build path `build/tools/vmaf` walked
   up from the test file. The test only skips when *no* binary is
   reachable, and the skip message names every probed location so
   CI debugging is a one-line task. The assertion shape (non-zero
   exit byte AND refusal stderr) is unchanged from V4-A.

2. **V5-2 container detection**: in `corpus.iter_rows`, derive
   `source_is_container = source.suffix.lower() not in
   _VMAF_RAW_SUFFIXES` and set the matching field on the
   `EncodeRequest`. When the source is a container the rung-target
   scale filter is always appended (`-vf scale=W:H`), giving
   ffmpeg an explicit rendition geometry regardless of the source's
   native resolution. Raw-YUV sources keep the legacy
   `-f rawvideo -pix_fmt … -s WxH` framing; the dedicated
   `test_iter_rows_keeps_raw_yuv_source_uncontainerised` guard
   pins the back-compat invariant.

3. **V5-3 full per-CRF sample cloud**: extend `make_default_sampler`
   and the underlying `_default_sampler` with an optional
   `cloud_sink: list[LadderPoint] | None` kwarg. When wired in, the
   sampler appends every successfully-scored CRF row from
   `iter_rows` into the sink *before* the `pick_target_vmaf`
   collapse. `build_and_emit` gains an `extra_samples` kwarg that,
   when present, supersedes the per-target `ladder.points` cloud as
   the source of the JSON `samples` array. The emit step calls a
   new `_dedup_samples` helper that drops `(width, height, crf)`
   repeats so the array is stable across sampler quirks (e.g. two
   targets that select the same CRF). The CLI's `_run_ladder`
   constructs the sink locally and threads it through both call
   sites — the legacy `build_and_emit` invocation without
   `extra_samples` still benefits from the dedup pass so V5-3 is
   fixed end-to-end.

## Alternatives considered

- **V5-2: probe ffprobe per cell to determine container shape.**
  Rejected: ffprobe is already invoked once per source for HDR
  detection (ADR-0300), but its output is needed *before* the
  encode argv is built. Switching from a cheap path-extension
  check to a probe wires an avoidable subprocess into every cell;
  the suffix check is correct for every input we ship and gives a
  caller-friendly error mode (a `.mp4` named `.yuv` opts into raw
  framing, matching the convention in `corpus._VMAF_RAW_SUFFIXES`).
- **V5-3: collect the cloud post-hoc by re-reading the corpus
  JSONL.** Rejected: the sampler writes the JSONL into a temp
  directory that's `tempfile.TemporaryDirectory`-managed, so the
  file is gone by the time `build_and_emit` runs. Persisting it
  would require a public emit-side dependency on the tempdir's
  lifetime, plus a re-parse cost; the sink list is a single-shot
  reference that costs O(rows) memory and zero I/O.
- **V5-3: change `pick_target_vmaf` to return the full sweep.**
  Rejected: `RecommendResult.row` is the bisect contract used by
  `vmaftune.bisect` and downstream consumers; widening it to a
  list would break ABI for every caller. The sink kwarg is opt-in
  and confined to the ladder pipeline.

## Consequences

- **Positive**:
  - `vmaf-tune ladder` against container sources now produces
    plausible VMAF (~85-95 on BBB 1080p, matching the
    single-source `compare` runs).
  - JSON `samples[]` carries every encoded CRF point per
    resolution, exactly once. Downstream consumers
    (`vmaf-tune report` Pareto overlay) see the full Pareto cloud
    instead of the per-target subset.
  - V5-1 regression test fires on every developer host that has
    built the binary, not just hosts where it landed on `$PATH`.
- **Negative**:
  - The container-source path adds a `-vf scale=W:H` filter to
    every encode regardless of whether the source is already at
    the rung target. The scale is a no-op for native-geometry
    rungs, but ffmpeg's filter graph still allocates buffers;
    dominated by the encode budget at < 1 % of wall time.
- **Neutral / follow-ups**:
  - `test_bbb_e2e_v5_bug_cluster.py` pins one regression per
    finding plus an opt-in end-to-end docker probe
    (`test_ladder_against_bbb_container_yields_plausible_vmaf`).
    The V4 `test_build_and_emit_threads_samples_into_json` is
    updated to reflect the new dedup'd sample-cloud invariant
    (`len(samples) == 2` for the all-same-CRF stub, not 4) —
    not a weakening, it's the V5-3 fix made testable on the v4
    fixture.
  - `docs/usage/vmaf-tune.md` documents the container-source
    encode behaviour and the full-cloud `samples[]` semantics.

## References

- BBB e2e v5 bug log: `/tmp/bbb_e2e_bugs_v5.md` (gitignored)
- Predecessor (v4): [ADR-0501](0501-vmaf-tune-bbb-e2e-v4-bug-cluster.md)
- Predecessor (v3): [ADR-0499](0499-vmaf-tune-ladder-reference-decode-v3.md)
- Predecessor (v2): [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)
- Encode driver: `tools/vmaf-tune/src/vmaftune/encode.py:build_ffmpeg_command`
- Corpus iter_rows: `tools/vmaf-tune/src/vmaftune/corpus.py:iter_rows`
- Ladder sampler + emit: `tools/vmaf-tune/src/vmaftune/ladder.py`
- CLI: `tools/vmaf-tune/src/vmaftune/cli.py:_run_ladder`
- Source: `req` (direct user direction in the agent dispatch
  briefing on 2026-05-18 — paraphrased: "fix the v5 bug cluster:
  vmaf vulkan strict-mode test must hit a real binary; ladder
  cross-res VMAF must be plausible; samples array must contain
  every CRF row, not just the per-target picks, and must not
  duplicate.")
