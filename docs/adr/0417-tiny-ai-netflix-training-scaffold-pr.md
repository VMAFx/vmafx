<!-- markdownlint-disable MD013 MD060 -->
# ADR-0417: Tiny-AI Netflix corpus training scaffold — draft PR registration

- **Status**: Accepted
- **Status update 2026-05-15**: implemented; PR #759 merged;
  `ai/data/netflix_loader.py` + `test_netflix_loader.py` present on
  master; Netflix corpus integration scaffolded (commit e7f524c44).
- **Date**: 2026-05-11
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `training`, `mcp`, `fork-local`, `onnx`, `docs`

## Context

[ADR-0242](0242-tiny-ai-netflix-training-corpus.md) (2026-04-27) established the
scaffold decision: document the corpus path convention, define the loader API,
record the architecture-choice space and distillation policy, and add an MCP
end-to-end smoke test — without running training. That decision was implemented
across several commits that landed on `master` before a dedicated scaffold PR was
opened.

This ADR formally registers the `ai/tiny-netflix-training-scaffold` draft PR. Its
purpose is to bundle the scaffold deliverables into a single reviewable unit so
the user can:

1. Confirm the architecture alternatives table (ADR-0242 §Alternatives considered)
   before committing to a training run.
2. Verify the MCP smoke test (`mcp-server/vmaf-mcp/tests/test_smoke_e2e.py`) passes
   against a local `build/tools/vmaf` binary before attaching Claude Code's MCP
   client to the server for interactive sessions.
3. Review the loader API and data-path safety invariants documented in
   `docs/ai/training-data.md`.
4. Read the updated literature survey in Research Digest 0099, which extends
   Digest 0019 with distillation-for-quality-metrics and ONNX Runtime 1.18+
   references from the 2024–2026 window.

The corpus itself (9 reference YUVs + 70 distorted YUVs) lives at
`.workingdir2/netflix/{ref,dis}/` on the user's local machine and is gitignored.
No training runs are triggered by merging this PR.

The fork's three Netflix CPU golden pairs (§ `CLAUDE.md §8`) are a held-out
correctness gate, not training data. That boundary is explicit in ADR-0242 and is
enforced by the `make test-netflix-golden` CI gate.

## Decision

We will open a draft PR on branch `ai/tiny-netflix-training-scaffold` that
delivers:

1. This ADR (0416) as the formal PR-level companion record, referencing ADR-0242
   as the original architecture-decision authority.
2. Research Digest 0099 (`docs/research/0099-tiny-ai-netflix-training-update.md`)
   extending Digest 0019 with 2024–2026 distillation and ONNX Runtime literature.
3. No changes to existing scaffold files already in `master`
   (ADR-0242, Digest 0019, `test_smoke_e2e.py`, `training-data.md`) — those are
   referenced, not duplicated.

Actual training, architecture selection, and hyperparameter choices are deferred to
a follow-up PR once the user has reviewed the ADR-0242 alternatives table and
confirmed architecture choices interactively.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Commit scaffold directly to `master` (no draft PR) | Faster path; no branch overhead | No review gate; user cannot confirm architecture before training run is triggered | Bypasses the deliberate review step ADR-0242 was designed to create |
| Merge scaffold + training run in one PR | Delivers a trained model immediately | Training is multi-day and GPU-bound; impossible to review the architecture choices mid-run | The corpus is local-only and CI cannot gate on training correctness anyway |
| Wait for explicit user instruction before opening the draft PR | Avoids noise from the daily routine | Delays surfacing the scaffold; the routine's purpose is exactly this daily readiness check | The idempotency check prevents duplicate PRs; a noop tomorrow if already open |
| Re-use ADR-0242 as the PR-companion record (no ADR-0417) | Fewer ADR files | ADR-0242 captures the architecture *decision*; this ADR captures the *PR registration* — different concerns | Conflates two distinct record purposes; ADR-0242 was accepted before the PR was opened |

## Consequences

- **Positive**: the user has a single reviewable PR URL as the trigger point for
  interactive architecture confirmation; the MCP smoke test is explicitly flagged
  as the one-command health check before attaching the MCP client.
- **Negative**: one additional ADR file for a procedural record rather than an
  architectural innovation; minor index maintenance cost.
- **Neutral / follow-ups**:
  - The follow-up PR to pick architecture, run training, export ONNX, and register
    the model under `model/` will cite this ADR and ADR-0242 as its decision
    ancestry.
  - ADR-0042 doc-substance rule applies to that follow-up PR: updated docs must
    land alongside the trained artefact.

## References

- [ADR-0242](0242-tiny-ai-netflix-training-corpus.md) — original scaffold
  architecture decision.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI doc-substance rule.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive deliverables.
- [Research Digest 0019](../research/0019-tiny-ai-netflix-training.md) — VMAF
  methodology survey and distillation literature (2026-04-27).
- [Research Digest 0099](../research/0099-tiny-ai-netflix-training-update.md) —
  2024–2026 distillation and ONNX Runtime update (this PR).
- `docs/ai/training-data.md` — corpus path convention, loader API, evaluation
  harness.
- `mcp-server/vmaf-mcp/tests/test_smoke_e2e.py` — MCP end-to-end smoke test.
- Source: `req` (daily prep-scaffolding routine, user instruction).
