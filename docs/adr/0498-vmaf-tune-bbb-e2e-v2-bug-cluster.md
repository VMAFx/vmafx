<!-- markdownlint-disable MD029 MD060 -->
# ADR-0498: vmaf-tune BBB end-to-end v2 bug cluster + explicit-backend semantics

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: `vmaf-tune`, `cli`, `libvmaf`, `bugfix`, `docs`, `container`

## Context

The follow-up BBB end-to-end smoke run (after PR #1253 / ADR-0497
closed the first seven-bug cluster) surfaced five new defects one
layer down: disk-runaway in the score-step decoder, a cross-resolution
ladder crash against a raw-YUV source, a missing `matplotlib`
dependency in the dev-mcp container image, a residual bare-`NaN`
literal in the report's JSON appendix, and a silent CPU fallback when
the libvmaf binary is asked for an explicit GPU backend that fails
to initialise. Two operational follow-ups (encoder availability
diagnostics and encoder-version detection) were grouped in the same
PR because they touch the same files and share a regression-test
fixture.

The decision was whether to ship a focused per-bug PR train or a
single consolidated cluster PR (precedent: ADR-0497 chose the
cluster form for the first BBB cluster).

## Decision

We ship a single PR (`fix/bbb-e2e-v2-bug-cluster-2026-05-18`) that
addresses all five v2 bugs plus the two operational follow-ups,
backed by nine regression tests in
`tools/vmaf-tune/tests/test_bbb_e2e_v2_bug_cluster.py`. The fixes
are:

1. **Bug #v2-A** (Major — disk runaway). `score._decode_to_raw_yuv`
   gained an optional `duration_s` parameter that emits a ffmpeg
   `-t` clamp on the output side. The duration is threaded down
   from a new `ScoreRequest.duration_s` field via the existing
   `maybe_decode_distorted` shim. `bisect._encode_and_score` and
   `corpus.iter_rows` populate it from the caller's
   `duration_s`. A 10 s probe against a 634 s 1080p source now
   produces ~896 MB of raw YUV instead of ~58 GB.
2. **Bug #v2-B** (Major — cross-resolution ladder crash). The
   default ladder sampler accepts new `src_width / src_height`
   parameters; when set and distinct from the rung target, the
   `CorpusJob` carries both source and rung dimensions and the
   `iter_rows` `EncodeRequest` builder switches to source-dim
   `-s W:H` plus a `-vf scale=W:H` filter for the downscale. The
   CLI defaults the source dims to the largest entry in
   `--resolutions` (so single-resolution ladders preserve the
   legacy behaviour) and exposes explicit `--src-width / --src-height`
   overrides for cases where the source resolution isn't one of the
   ladder rungs.
3. **Bug #v2-C** (Major — container surface gap, ADR-0496
   compliance). `dev/Containerfile` now `pip install`s `matplotlib`
   into `/opt/vmaf-venv` so `vmaf-tune report` works inside the
   container without an out-of-band `pip install`. As defence in
   depth, `report.py`'s chart helpers fall back to an HTML comment
   placeholder when matplotlib is unimportable, so table-only
   reports still render outside the container.
4. **Bug #v2-D** (Minor — RFC 8259 conformance). `report.py`'s
   `<details>` JSON appendix (both Markdown and HTML paths) now
   passes `allow_nan=False` and coerces `float('nan')` /
   `float('inf')` to `None`. The fix mirrors the `_nan_to_none`
   shim already in `compare.py` (added by ADR-0497) so strict JSON
   parsers (Go, Rust, `jq`) accept the appendix.
5. **Bug #v2-E** (Major — silent backend fallback). The vmaf
   binary's `init_gpu_backends` derives an `explicit_backend` flag
   from `--backend NAME` (anything other than `auto` / `cpu`) and
   turns each per-backend `state_init` failure into a non-zero
   exit when that backend was the explicit request. The
   `--backend auto` path keeps the legacy soft-fallback chain.
   Independently, the JSON output now carries a top-level
   `"backend_used": "NAME"` key (cpu / cuda / sycl / vulkan / hip /
   metal) so CI gates and MCP probes can confirm what actually
   ran — mirrors the MCP-layer echo added by PR #1251.

Operational follow-ups, same PR:

6. **Encoder availability vs encode failure** — `bisect._encode_and_score`
   now distinguishes "Encoder not found" / "Unknown encoder" stderr
   markers and reports `encoder unavailable (libsvtav1): …` instead
   of the cryptic `encode failed at CRF NN (exit=1): Encoder not
   found` the pre-ADR-0498 path emitted.
7. **Encoder version detection** — `encode.parse_versions` regex
   widened to accept the `x264 - core 164` / `x264-core 164`
   variants, and a process-cached `_probe_encoder_version_from_ffmpeg`
   helper falls back to `ffmpeg -version`'s `--enable-libx264` /
   `--enable-libsvtav1` configure-line markers when the per-encoder
   banner is suppressed by `-hide_banner`. Rows that previously
   carried `"unknown"` now carry `libx264-enabled` /
   `libsvtav1-enabled` so consumers can at least confirm the
   encoder is compiled in.
8. **dev-mcp-stdio /tmp** — `dev/scripts/dev-mcp-entrypoint.sh`
   `mkdir -p /tmp && chmod 1777 /tmp` as its first action so the
   sibling MCP log + the bug-cluster repro scripts never fail on
   "No such file or directory: /tmp/vmaf-mcp.log" when the runtime
   ships a minimal `/` filesystem.

The "consolidated cluster" form was chosen over per-bug PRs for the
same reasons ADR-0497 cited: bugs were discovered together by one
repro, they all gate the same documented headline workflow, the
regression-test fixture is shared, and per-bug PRs would multiply
CI cost.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Five separate PRs (one per major bug) | Smaller diffs, atomic reverts | 5× CI cost, fragmented test fixture, harder to verify the e2e smoke went green | Bugs share a v2 cluster identity; splitting hides the regression context |
| Bug #v2-E: extend the libvmaf C API with a `vmaf_get_active_backend()` getter | Cleaner integration, no JSON post-edit | Touches a public header (triggers ffmpeg-patches rebase per rule 14), and the MCP layer already echoes `backend_used` (PR #1251) — the same data is available there | Out-of-process textual amend keeps the API surface stable; downstream consumers already parse JSON, not C |
| Bug #v2-B: reject multi-resolution YUV ladders with a clear error and require container sources | Smaller diff | Punishes users who legitimately have raw YUV at a higher resolution than their target ladder; closes off a documented workflow | Cross-resolution sampling against a raw source is a normal authoring step; the fix is straightforward (scale filter) and unblocks the workflow |
| Bug #v2-C: skip charts when matplotlib is missing, don't add the dep | Smallest container delta | Charts are part of the documented report output; ADR-0496 says every user surface works in the container | Adopted the dep AND added the fallback — the container is the supported path; the fallback is for host-side runs |

## Consequences

### Positive

- The documented `vmaf-tune` headline workflow now survives a
  realistic re-probe (10 s window against 634 s 1080p source) on a
  modest dev-mcp host without disk-space drama.
- Cross-resolution ladders against raw YUV sources work as
  documented.
- `vmaf-tune report` works inside `vmaf-dev-mcp` without
  out-of-band pip-installs (ADR-0496 compliance).
- The JSON output of both `compare`, `ladder`, and `report` is now
  uniformly RFC 8259 conformant.
- CI gates depending on `--backend NAME` now fail loudly when the
  backend can't init, instead of silently regressing to CPU
  scoring. The new `backend_used` JSON key lets downstream tooling
  confirm dispatch independently of the human-readable stderr.

### Negative / costs

- `ScoreRequest` and `CorpusJob` grew new optional fields; the
  defaults preserve the legacy behaviour but pinned-shape
  consumers (tests that compare against `dataclasses.asdict`)
  may need a one-line update.
- The dev-mcp container is one matplotlib install heavier
  (~50 MB of dependencies). Worth it per ADR-0496.
- The `init_gpu_backends` C function gained ~25 lines of
  explicit-backend gating, pushing it deeper into NOLINT-
  function-size territory; the existing NOLINTNEXTLINE +
  ADR-0141 citation already cover it.

### Neutral

- `_probe_encoder_version_from_ffmpeg` runs at most once per
  process per (binary, encoder) tuple; the cache lives in module
  scope so test isolation requires explicit `_PROBE_CACHE.clear()`
  (mirrors existing patterns).

## References

- Bug log: `/tmp/bbb_e2e_bugs_v2.md` (BBB e2e v2 probe report,
  2026-05-18).
- ADR-0497: prior BBB end-to-end cluster (closed seven bugs;
  this ADR closes the next layer of five).
- ADR-0496: prefer `vmaf-dev-mcp` container for vmaf / vmaf-tune /
  ai / MCP-probing work — drives the matplotlib container fix.
- ADR-0495 / PR #1251: MCP-layer `backend_used` echo — mirrored
  by Bug #v2-E's JSON-output amend.
- ADR-0299 / ADR-0175 / ADR-0186 / ADR-0299: backend dispatch
  contract — explicit-backend semantics are an explicit
  documentation refinement.

paraphrased: per user direction "fix all 5 v2 bugs found by the
second BBB end-to-end probe. Single PR" — five major fixes plus
three operational follow-ups consolidated in one PR per the
"bigger-content PRs over per-LOC PRs" rule.
