<!-- markdownlint-disable MD013 MD060 -->
# ADR-0682: Tiny-AI Netflix corpus training scaffold — 2026-05-22 prep scope

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `training`, `fork-local`, `onnx`, `mcp`, `docs`

## Context

ADR-0242 (2026-04-27) defined the scaffold-only PR strategy for training tiny-AI
full-reference regressors on the original Netflix VMAF corpus
(`.workingdir2/netflix/{ref,dis}/`, 9 reference + 70 distorted YUVs, gitignored).
Subsequent iterations (ADR-0612, ADR-0640) refreshed the research digest and extended
the architecture alternatives table through 2026-05-20.

This ADR records the 2026-05-22 prep-scope update, triggered by the merge of
PR #152 (`fix/volk-static-archive-priv-remap`, ADR-0198), which was the gate the
training-scaffold routine waited on before opening its canonical draft PR on the
`ai/tiny-netflix-training-scaffold` branch.

Three open questions remain from the prior iterations:

1. **Architecture selection**: the 2×64-nano MLP baseline vs the 3×128-tiny MLP vs
   the attention-pooled variant. No architecture has been selected.
2. **Distillation vs from-scratch**: the decision table in ADR-0242 lists both as
   viable; no option has been chosen.
3. **MCP smoke-test health**: the `test_smoke_e2e.py` harness exercises the
   `vmaf_score` JSON-RPC path against the Netflix golden fixture. Users must verify
   the binary path (`build/tools/vmaf`) is present before the training workflow
   starts.

## Decision

We will open branch `ai/tiny-netflix-training-scaffold` as a consolidated draft PR
that:

1. Ships ADR-0682 (this file) as the 2026-05-22 scope record, cross-referencing
   ADR-0242 as the root decision.
2. Adds Research Digest 0706, surveying 2025–2026 lightweight FR metric training
   literature relevant to the architecture-selection decision still open from ADR-0242.
3. Records a `no state delta: feature scaffold, no bug closed/opened` note in
   `docs/state.md` (PR description field).
4. Adds a rebase-notes entry documenting the corpus path invariant and the
   `ai/tiny-netflix-training-scaffold` branch name so sibling agents do not
   re-scaffold.
5. Does NOT run training, download corpus data, or touch Netflix golden test
   assertions (CLAUDE.md §8).

The draft PR is the formal review gate. Architecture selection and the first actual
training run remain deferred; they will land in a follow-up PR after the user confirms
the architecture choice via the popup workflow.

## Alternatives considered

| Option | Pros | Cons | Status |
|---|---|---|---|
| Open the PR immediately after PR #152 merges (this ADR) | Unblocks architecture-selection discussion; gives user a single reviewable object | Adds another ADR without new code | **Chosen** |
| Wait for architecture selection before opening any PR | Fewer PRs; single coherent commit | Delays the formal review gate indefinitely; user loses the scaffold state across context resets | Rejected |
| Merge scaffold content directly to master without a draft PR | Fewer branch management steps | Violates CLAUDE.md §12 rule 3 (no direct commits to master); no formal review point | Rejected |
| Use an existing branch (e.g. a `feat/ai-training-*` sweep branch) | Reuses existing work | Name mismatch breaks the routine's idempotency check; scope conflation | Rejected |

## Consequences

- **Positive**: the `ai/tiny-netflix-training-scaffold` branch exists, satisfying the
  routine's idempotency check for all subsequent daily runs.
- **Positive**: the draft PR is the natural place for architecture-selection discussion
  between the user and the routine agent.
- **Negative**: the ADR count grows by one without new code shipping.
- **Neutral / follow-ups**:
  - Architecture selection PR (follow-up, pending user popup response).
  - First training run PR (multi-day GPU job, `--data-root .workingdir2/netflix/`).
  - CI cannot validate the corpus path (gitignored); manual pre-run check is required.

## References

- ADR-0242: [Tiny-AI training on the original Netflix VMAF corpus](0242-tiny-ai-netflix-training-corpus.md) — root decision.
- ADR-0612, ADR-0640: prior 2026-05-19 and 2026-05-20 research iterations.
- ADR-0198: [volk static-archive priv remap](0198-volk-priv-remap-static-archive.md) — the gate PR that unblocked this scaffold.
- Research-0019: [Tiny-AI Training on the Netflix VMAF Corpus](../research/0019-tiny-ai-netflix-training.md) — original methodology survey.
- Research-0706: [Tiny-AI Netflix Training Prep — 2026-05-22 literature update](../research/0706-tiny-ai-netflix-training-prep-2026-05-22.md) — this PR's digest.
- User memory entry: `project_netflix_training_corpus_local.md` (corpus layout, local path, encoding-ladder naming convention).
- PR #152: `fix/volk-static-archive-priv-remap` — the gate that unblocked this routine.
