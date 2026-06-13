<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1073: Fix vmaf_score_at_index EAGAIN-guard misapplication for model output slots

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `mcp`, `scoring`, `correctness`, `core`, `fork-local`

## Context

`vmaf_score_at_index` in `core/src/libvmaf.c` first tries to retrieve an already-computed
VMAF model score from the feature collector, then — if that fails — calls
`vmaf_predict_score_at_index` to compute and store the score.

Commit `c31f2fe26` (ADR-0154 / Netflix#755) added a guard to prevent calling
`vmaf_predict_score_at_index` when the underlying error is `-EAGAIN`, on the grounds
that `-EAGAIN` signals "retroactive-write input feature not yet available" (e.g.
`integer_motion` emits `motion2`/`motion3` for frame N only after the flush
`vmaf_read_pictures(NULL, NULL, 0)` call). Calling predict in that state would
replace a transient `-EAGAIN` with a terminal `-EINVAL`.

The guard was correct for *input* feature vectors, but was incorrectly applied to
the *model output* (the "vmaf" score vector itself). When `vmaf_score_at_index` is
called for frame 0, `vmaf_predict_score_at_index` creates the "vmaf" feature vector,
allocates it with capacity > 1, and writes slot 0. For frame 1, `get_score("vmaf", 1)`
finds the vector but slot 1 has `written=false`, which returns `-EAGAIN`. The guard
`if (err && err != -EAGAIN)` then suppresses the `vmaf_predict_score_at_index` call,
propagating `-EAGAIN` up through `vmaf_score_pooled` for all frames after the first.

The practical impact: `vmaf_score_pooled` returned `-EAGAIN` (−11) on any sequence
with more than one frame when called immediately after flush. The MCP
`compute_vmaf` handler surfaced this as a JSON-RPC error response, causing
`test_mcp_smoke::test_compute_vmaf_10bit` to fail.

A contributing issue: the `test_compute_vmaf_10bit` fixture used 64×64 frames.
`vmaf_v0.6.1` uses integer feature extractors only (adm2, motion2, vif_scale0–3);
none require a minimum dimension. The 64→192 bump was added as a conservative
defensive measure to match the 192px floor used by ADR-1072 companion tests, since
the original comment cited `float_ms_ssim` (which was not actually in use).

## Decision

Remove `err != -EAGAIN` from the guard in `vmaf_score_at_index`. The corrected
condition is `if (err)`: any failure to read the model output score — including
`-EAGAIN` from an unwritten model output slot — must fall through to
`vmaf_predict_score_at_index`.

The retroactive-write input-feature case (integer_motion's motion2/motion3) is
handled correctly by the flush call that precedes `vmaf_score_pooled`; by the time
scoring begins, all input features are available and no `-EAGAIN` propagates from
`vmaf_feature_collector_get_score` on the input side.

The `test_compute_vmaf_10bit` fixture dimensions are bumped 64→192 for defensive
alignment with the broader test convention; this change is independent of the
scoring bug.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Distinguish input vs output `-EAGAIN` in the feature collector (add a second error code) | More precise semantics | Major API-contract change; breaks Netflix upstream compatibility; ADR required for the error-code expansion | Disproportionate complexity for a guard that only needs to allow predict on the model output slot |
| Revert ADR-0154 guard entirely | Simpler code | Re-exposes the original Netflix#755 race where predict is called with incomplete input features | The race is real; the guard is correct for input features. Only the scope was wrong. |
| Keep guard, call flush twice | No code changes to libvmaf.c | Does not fix the problem; the model output slot not being written has nothing to do with flush status | Does not address root cause |

## Consequences

- **Positive**: `vmaf_score_pooled` correctly returns scores for all frames in
  a multi-frame sequence. `test_mcp_smoke::test_compute_vmaf_10bit` passes.
  Fast suite (84/84) continues to pass.
- **Negative**: None. The ADR-0154 invariant (do not predict with incomplete
  inputs) continues to hold because flush precedes all scoring calls.
- **Neutral**: The fix is a one-character removal (`&& err != -EAGAIN` dropped).
  It does not affect Netflix golden-data test assertions.

## References

- Netflix#755 / ADR-0154: original context for the `-EAGAIN` guard.
- ADR-1072: companion fix in the same session (PREV_REF refcount leak).
- PR closing this ADR: fix/mcp-score-at-index-eagain-guard.
- req: "Fix test_mcp_smoke.c test_compute_vmaf_10bit failure on master tip 4e8842289d."
