<!-- markdownlint-disable MD060 -->
# ADR-0497: vmaf-tune BBB end-to-end bug cluster (compare / ladder / report)

- **Status**: Accepted
- **Date**: 2026-05-17
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: `vmaf-tune`, `cli`, `bugfix`, `docs`

## Context

The 2026-05-17 Big Buck Bunny end-to-end smoke run on `vmaf-tune`
([report](../research/2026-05-17-vmaf-tune-bbb-e2e-bug-log.md))
surfaced seven distinct defects that broke the documented
`compare → ladder → tune-per-shot → report` pipeline against a real
1080p30 container source. The defects spanned three modules and one
shared invariant (libvmaf only accepts raw `.yuv` / `.y4m`); each one
individually would have been a small fix but together they made the
documented headline workflow non-functional on the canonical example
input that the `--help` text advertises (raw YUV "*or any
FFmpeg-readable container*"). The decision was whether to ship a
focused per-bug PR train or a single consolidated cluster PR.

## Decision

We ship a single PR (`fix/vmaf-tune-bbb-e2e-bugs-2026-05-17`) that
addresses all seven bugs together, plus six regression tests that
pin the cluster in `tools/vmaf-tune/tests/test_bbb_e2e_bug_cluster.py`.
The fixes are:

1. **Bug #3 / #7** (highest priority — blocks all real `compare` and
   `tune-per-shot` scoring): the bisect now decodes the encoded
   container (`.mkv`) to raw YUV before invoking the vmaf CLI. The
   helper `score.maybe_decode_distorted()` was promoted to a public
   shared function (lifted from the private `corpus._maybe_decode_distorted`
   pattern) so the post-encode decode step is one call site, not
   three. A new `decode_runner` parameter on `bisect_target_vmaf` /
   `make_bisect_predicate` keeps the subprocess seam testable.
2. **Bug #1**: container sources (`.mp4` / `.mkv`) are now
   autodetected via the suffix table; the `EncodeRequest` carries
   `source_is_container=True` so the encoder ffmpeg invocation skips
   the `-f rawvideo -pix_fmt … -s … -r …` input flags. Without this
   fix, ffmpeg parses the demuxed mp4 as raw YUV and emits zero
   frames.
3. **Bug #2**: `compare --format json` no longer writes bare `NaN` /
   `Infinity` tokens. `_emit_json` substitutes `None` (→ `null`) for
   non-finite floats and passes `allow_nan=False` as defence in
   depth. Strict RFC 8259 parsers (Go, Rust, `jq --strict`) now load
   the output cleanly; failed rows are still distinguishable via the
   per-row `ok` flag.
4. **Bug #4**: the `ladder` CLI now exposes `--framerate` /
   `--duration` / `--pix-fmt` flags symmetric with `compare` /
   `tune-per-shot`. The factory `ladder.make_default_sampler(...)`
   binds them into the default corpus sampler so bitrate math and
   encode timing match the real source shape instead of the legacy
   hardcoded `(24.0, 1.0, yuv420p)` triple.
5. **Bug #5**: a new `--crf-sweep 23,28` flag overrides the
   module-level `DEFAULT_SAMPLER_CRF_SWEEP`. Smoke runs and the
   smaller "BBB e2e verify" recipe in CI can now exercise the ladder
   plumbing with a 1–2-CRF schedule instead of the production
   5-point grid.
6. **Bug #6**: `report` now treats `NaN` / `None` numerics as
   missing data. The renderer formats them as `—` (em-dash) instead
   of `nan kbps`; the JSON-load step in `cli._codec_row_from_json`
   coerces non-finite inputs to `NaN` rather than `0.0`; the chart
   plotter drops non-finite points so matplotlib doesn't see them;
   and the CLI's stdout summary now aggregates `ok` from row-level
   flags (`ok=false` when any input row failed) plus exposes
   `codec_rows_ok` / `codec_rows_failed` counts.

The "consolidated cluster" form was chosen over per-bug PRs because:
the bugs were discovered together by one repro, they all gate the
same documented headline workflow, and the test fixture
(`_make_bisect_stubs`) is shared across the regression tests. Per-bug
PRs would have multiplied CI cost, fragmented the test fixture, and
made the rebase / revert story noisier (see the "Bigger-content PRs
over per-LOC PRs" rule in user memory).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Seven separate PRs (one per bug) | Smaller diffs, atomic reverts | 7× CI cost, fragmented test fixture, harder to verify the e2e smoke went green | Bugs share a root cause (container vs raw demarcation) and a shared regression test fixture; splitting hides the cluster |
| Single PR, bisect emits raw YUV from the encoder | Avoids the extra decode step | Codec-adapter-specific shape change (would need raw output support per encoder), and the encoded artefact is still useful as a sanity check | The decode step is a 1-call ffmpeg op (~ms on small samples) and matches what `corpus.py` already does — no precedent to invent |
| Auto-probe `--framerate` / `--duration` via ffprobe in `ladder` | One flag fewer to set | Hides defaults behind a 2nd probe call, breaks the symmetry with `compare` / `tune-per-shot` which take explicit flags | Symmetric explicit flags is the documented surface; ffprobe fallback is a future enhancement (see also the `auto` subcommand which already probes) |
| Render failed rows as a separate "errors" table | Visually clearer | Doubles the rendering code path, breaks `jq` consumers that key off the existing table shape | Em-dash + per-row `ok` flag preserves the schema while making the failure visible |

## Consequences

- **Positive**:
  - The documented `compare → ladder → tune-per-shot → report`
    workflow now runs end-to-end against a real `.mp4` source
    without manual pre-extraction to raw YUV.
  - Compare JSON output validates as strict RFC 8259 — downstream
    Go / Rust consumers no longer need bespoke NaN parsers.
  - Report stdout `ok` flag is now trustworthy under `jq .ok`; the
    `codec_rows_ok` / `codec_rows_failed` counts let CI gate on
    "all rows succeeded" without re-parsing the rendered card.
  - Ladder smoke runs can use a 1–2-CRF sweep, dropping wall-time
    from minutes to seconds for plumbing-only verification.
- **Negative**:
  - The new `decode_runner` parameter on `bisect_target_vmaf` /
    `make_bisect_predicate` is an API surface widening; tests that
    inject a custom encode runner already get the decode invocation
    routed to it via the runner fallback, so the practical impact
    is one extra optional kwarg.
  - The bisect now does an extra ffmpeg decode per iteration for
    every container source. The decoded-reference is cached across
    iterations within the same bisect (single decode per source,
    not per-CRF), so the cost is bounded; on small sample clips
    it's <100 ms.
- **Neutral / follow-ups**:
  - `corpus.py::_maybe_decode_distorted` is now a partial duplicate
    of the lifted `score.maybe_decode_distorted`; a follow-up
    refactor can collapse the two. Out of scope here — the corpus
    path was working correctly, only bisect was broken.
  - `tune-per-shot` is exercised by the unit test
    (`test_tune_per_shot_score_step_decodes_container`) but not by
    a live BBB run (the per-shot binary is not on the dev PATH).
    Bug #3 propagates through the shared bisect, so the fix is
    structural; the per-shot live run is a future verification
    item.

## References

- BBB e2e bug log: `/tmp/bbb_e2e_bugs.md` (paraphrased; the
  user-direction PR brief was "fix the 7 vmaf-tune bugs the BBB
  end-to-end agent found, single PR").
- Six regression tests in
  `tools/vmaf-tune/tests/test_bbb_e2e_bug_cluster.py`.
- Related: ADR-0322 (Phase B target-VMAF bisect), ADR-0295 (Phase E
  per-title ladder), ADR-0307 (default CRF sweep), ADR-0301
  (sample-clip encoding).
- Source: `req` — verbatim PR brief in the agent dispatch message.
