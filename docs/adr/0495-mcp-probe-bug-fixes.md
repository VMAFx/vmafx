<!-- markdownlint-disable MD060 -->
# ADR-0495: MCP server probe-driven bug-fix cluster (2026-05-17)

- **Status**: Accepted
- **Date**: 2026-05-17
- **Deciders**: lusoris
- **Tags**: mcp, ai, testing, regression-recovery

## Context

A scripted probe of the `vmaf-dev-mcp-stdio` container against
`vmaf 3.0.0` (driver at `/tmp/mcp_probe.py`, full triage at
`/tmp/mcp_probe_findings.md`) surfaced five concrete defects in the
MCP surface:

1. **Silent backend fallback.** `vmaf_score` with `backend="cuda"` on a
   CPU-only build returned success with bit-identical CPU scores, no
   warning, no `backend_used` field. Cross-backend parity tests built on
   top of MCP would silently report "CUDA == CPU bit-exact" while never
   running CUDA.
2. **Schema enum drops `vulkan` / `hip` / `metal`.** Both `vmaf_score`
   and `describe_worst_frames` rejected backends that the underlying
   binary and the server's own `_BACKEND_DISABLE` map already supported.
3. **`run_benchmark` returns `exit_code=1` with empty stdout AND
   stderr.** `testdata/bench_all.sh` aborts before flushing output
   under `set -euo pipefail` (typically when the vmaf binary is missing
   or `source setvars.sh` is non-zero). The MCP wrapper returned the
   success-shaped envelope with two empty strings, leaving the caller
   no way to diagnose the failure.
4. **`ref == dis` ≠ 100.** Scoring an 8-bit YUV against itself yields
   ~97.43 on 47/48 frames; per-feature integers are all 1.0. This is a
   property of the `vmaf_v0.6.1` model's negative-bias regressor — the
   trained polynomial does not saturate to 100 even when all feature
   inputs are perfect. Documented as a model artefact below; no code
   fix.
5. **`vmaf_4k_v0.6.1` on 576×324 returns 100 on every frame.** The 4K
   model silently saturates on sub-1080p sources because its activation
   range was fit against 3840×2160 statistics; the MCP wrapper passed
   the bogus pool through with no hint.

## Decision

We will:

- Probe the local vmaf binary's `--help` output at first
  `vmaf_score` call (cached for the process lifetime) and **refuse**
  any caller-requested backend that is not advertised — instead of
  silently letting the binary fall back to CPU. The response now
  includes `backend_requested` and `backend_used` fields on every
  successful call so downstream parity tests can assert which backend
  actually ran.
- Add `vulkan`, `hip`, `metal` to the `backend` enum of both
  `vmaf_score` and `describe_worst_frames`. The `_BACKEND_DISABLE` map
  already supported these — only the JSON-schema surface had drifted.
- Extend the `run_benchmark` wrapper to detect the
  "non-zero exit + empty stdout + empty stderr" pattern and populate a
  meaningful `error` field with the most common root-cause shortlist
  (missing vmaf binary, oneAPI setvars, missing fixtures) plus the
  `bash -x` re-run hint. The benchmark script itself now exits `2`
  with a clear stderr message when the vmaf binary is missing,
  removing the most common silent-abort path.
- Emit a `mismatched_model_warning` field in the `vmaf_score` response
  when the model's intended resolution preset (`hd` / `4k` / `sd`)
  disagrees with the source frame size's bucket. The classifier
  understands the in-tree models (`vmaf_v0.6.1`, `vmaf_v0.6.1neg`,
  `vmaf_b_v0.6.3` → `hd`; anything with `vmaf_4k` → `4k`) and stays
  silent on bespoke ONNX models we have no resolution metadata for.
- Document Bug #4 as a model artefact rather than patching scores: the
  Netflix `vmaf_v0.6.1` model is the golden-data ground truth
  (ADR-0024, CLAUDE.md §8) — clamping at the MCP layer would diverge
  the wrapper from the libvmaf CLI and from the Netflix assertions
  we are forbidden to modify.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Use `--backend $name` exclusive selector instead of the `--no_*` flag set | Single canonical CLI surface, matches `bench_all.sh` documentation | Doesn't work on older fork builds that predate the selector (2026-04-28); breaks `vmaf-dev-mcp-stdio` containers built against older snapshots | Probe-and-refuse via `--no_*` works on every fork build that ever shipped |
| Clamp `vmaf_v0.6.1` scores `>100 → 100`, `<0 → 0` at the MCP layer (Bug #4) | Caller intuition "identical YUV ⇒ 100" satisfied | Would diverge MCP scores from the libvmaf CLI and the Netflix golden gate — exactly the assertion class CLAUDE.md §8 forbids modifying | Documented as a model artefact; agents and humans who need a 100-on-identical pair should pick the `vmaf_v0.6.1neg` model (which clips) |
| Hard-fail on 4K-model + SD-source instead of warning (Bug #5) | Loud, unmissable | Breaks legitimate use cases (smoke-testing the 4K model on tiny fixtures during dev); too rigid a policy for a per-call tool | Warning is the right register — surfaces the foot-gun without removing capability |

## Consequences

- **Positive**: agents using `vmaf_score` for cross-backend parity work
  now get an actionable error instead of a false "CUDA == CPU"
  conclusion; `backend_used` lets parity tests assert against the
  binary's actual selection; `run_benchmark` failures are debuggable;
  resolution-mismatch foot-guns surface a warning early.
- **Negative**: callers that previously relied on the silent-fallback
  behaviour ("just give me CPU scores even when I asked for CUDA")
  must now explicitly pass `backend='auto'` or `'cpu'`. This is a
  breaking change in the wire contract; documented in
  `docs/mcp/tools.md`.
- **Neutral / follow-ups**: 5 regression tests added under
  `mcp-server/vmaf-mcp/tests/test_probe_findings_2026_05_17.py`. Doc
  updates to `docs/mcp/tools.md`. Changelog fragments under
  `changelog.d/fixed/`. Probe driver and findings file are
  process-local (`/tmp/`) and not in-tree.

## References

- Source: `req` ("Fix all 5 bugs the MCP probe agent found. Single PR.")
- Probe triage: `/tmp/mcp_probe_findings.md` (process-local).
- Related ADRs: [ADR-0172](0172-mcp-describe-worst-frames.md),
  [ADR-0119](0119-cli-precision-default-revert.md),
  [ADR-0024](0024-netflix-golden-tests.md) (Bug #4 rationale).
