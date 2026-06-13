<!-- markdownlint-disable MD060 -->
# ADR-0501: vmaf-tune ladder cross-resolution scoring + report degraded flag

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: vmaf-tune, ladder, corpus, report, vmaf-cli, docs

## Context

The BBB end-to-end v4 probe (after PR #1257 / ADR-0499 closed the v3
cluster) surfaced three findings:

- **V4-A** — `vmaf --backend vulkan` against a Vulkan-less runtime
  prints the ADR-0498 strict-mode refusal *and* (per the v4 bug log)
  was reported as exiting `0`. A re-run on the current dev-mcp
  binary (built 2026-05-17) shows the propagation chain
  `init_gpu_backends() -> ret = -1 -> main() -> exit(255)` is in
  fact working, but the contract had no regression test pinning it.
  The risk was a future refactor of `main()`'s ret-chain silently
  re-introducing the failure.
- **V4-B** — `vmaf-tune ladder --resolutions 1920x1080,1280x720` on
  the BBB 1080p mp4 produced a single rung with `vmaf=21.3 @ 23 Mbps
  720p`, which is implausible (`compare` at the same CRF returns
  ~93). Root cause: `vmaftune.corpus._maybe_decode_reference` (added
  in ADR-0499) decodes the reference at the source's native
  geometry, but the rung's `ScoreRequest` then tells the libvmaf
  CLI to read both legs at the *rung target* (1280x720). The binary
  silently mis-parses the 1080p planar bytes as a 720p frame and
  emits garbage VMAF — a value low enough that the post-hull select
  collapses the grid to a single rendition. The encoded distorted
  leg was already scaled correctly via the `-vf scale=W:H` filter
  added in ADR-0498; the reference leg never got the matching
  treatment. Separately, the JSON descriptor only emitted
  `renditions[]` — the `report` consumer's `ladder_samples` counter
  always read `0` because no `samples[]` array was wired.
- **V4-C** — `vmaf-tune report` aggregated `ok=true` only when every
  codec row was `ok=true`. The ADR-0498 bisect discriminator marks
  rows for unavailable encoders as
  `error="encoder unavailable (NAME): …"` — an *infrastructure gap*,
  not a quality failure. Conflating the two flips the report to
  `ok=false` purely because the dev-mcp ffmpeg ships without
  `libsvtav1`, masking real regressions in the same field.

## Decision

1. **V4-A pin**: add a Python integration test under
   `tools/vmaf-tune/tests/` that runs `vmaf --backend vulkan`
   against the Netflix golden-checkerboard YUVs and asserts a
   non-zero exit byte plus the refusal message in stderr. The test
   skips when the binary isn't on `$PATH` or when the host has a
   working Vulkan device (the refusal path doesn't engage on real
   hardware).
2. **V4-B**: extend `_maybe_decode_reference` (and its
   `_decode_source_to_yuv` building block) with optional
   `target_width` / `target_height` kwargs that append `-vf
   scale=W:H` to the ffmpeg decode argv and embed the dims in the
   sidecar filename so multi-rung sweeps don't collide. Wire
   `iter_rows` to pass the rung target whenever
   `CorpusJob.src_width/src_height` differs from `width/height`.
   Extend `emit_manifest` / `_emit_json` to accept a `samples=`
   sequence and emit a top-level `samples[]` array in the
   `vmaf-tune-ladder/v1` JSON; thread the pre-hull sampler cloud
   through from `build_and_emit`.
3. **V4-C**: split row-level failures in `_run_report` into "real
   failures" (anything whose `error` does *not* start with
   `"encoder unavailable"`) and "unavailable rows". `ok` gates only
   on real failures + at-least-one-success; a new top-level
   `degraded` field flips `true` whenever any unavailable row is
   present. `codec_rows_unavailable` is added so dashboards can
   show the count without parsing every row's `error`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **V4-B**: decode reference at native geometry once and pass dims to libvmaf separately | One decode for all rungs; less I/O | Requires a new libvmaf CLI flag pair (`--ref_width` / `--ref_height`) — the existing single `--width` / `--height` apply to *both* legs; would gate on a separate libvmaf PR and break Netflix-golden CLI parity | Per-rung scale matches what we already do for the distorted leg; symmetric, no new flag |
| **V4-B**: emit `samples` only when caller passes a non-empty list | Smaller JSON when uncalled | KeyError on downstream `report` consumers; inconsistent schema | Always-emit (possibly empty) array; stable schema beats marginal byte savings |
| **V4-C**: ignore unavailable rows entirely (drop from `codec_rows_failed`) | Cleaner ok=true semantics | Hides the infrastructure gap from dashboards; operators can't see which encoder is missing | New `degraded` flag + dedicated counter — keeps the signal, removes the false negative |
| **V4-C**: classify on a row-level `error_kind` enum instead of a string prefix | Stable taxonomy | Requires schema migration of every existing compare JSON consumer | String prefix is a pragmatic 1-PR fix; an enum migration can land later if more error kinds appear |

## Consequences

- **Positive**: `vmaf-tune ladder` produces plausible VMAF on
  cross-resolution rungs against container sources. `report`
  consumers can render the Pareto cloud (samples) and a single PR
  no longer flips `ok=false` purely because an optional encoder
  isn't built into the local ffmpeg.
- **Negative**: One extra ffmpeg decode per *resolution* (not per
  cell — the per-rung sidecar is reused across the CRF sweep)
  when the source dims differ from the rung target. For a typical
  5-rung ladder against a 1080p source that's 4 extra decodes;
  dominated by the encode budget at < 1 % of wall time.
- **Neutral / follow-ups**:
  - `test_bbb_e2e_v4_bug_cluster.py` pins one regression per
    finding. The existing V2 cross-res scale-filter test was
    extended to also assert the reference-side scale invocation
    (the V2 fix didn't realise the reference was a parallel
    issue).
  - `docs/usage/vmaf-tune.md` documents the cross-res reference
    decode under "Container and Y4M sources" and the new
    `samples[]` / `degraded` outputs under the JSON descriptor
    section.

## References

- BBB e2e v4 bug log: `/tmp/bbb_e2e_bugs_v4.md` (gitignored)
- Predecessor (v3): [ADR-0499](0499-vmaf-tune-ladder-reference-decode-v3.md)
- Predecessor (v2): [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)
- Predecessor (v1): [ADR-0497](0497-vmaf-tune-bbb-e2e-bug-cluster.md)
- Container build policy: [ADR-0496](0496-prefer-dev-mcp-container-rule.md)
- Reference-decode helper: `tools/vmaf-tune/src/vmaftune/corpus.py:_maybe_decode_reference`
- Report aggregation: `tools/vmaf-tune/src/vmaftune/cli.py:_run_report`
- JSON emitter: `tools/vmaf-tune/src/vmaftune/ladder.py:_emit_json`
- Source: `req` (direct user direction in the agent dispatch
  briefing on 2026-05-18 — paraphrased: "fix the v4 bug cluster:
  vmaf vulkan strict-mode must exit non-zero; ladder grid must not
  collapse and must produce plausible VMAF + a samples array; report
  must distinguish encoder-unavailable from encode-failed").
