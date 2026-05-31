<!-- markdownlint-disable MD013 MD056 MD060 -->
# ADR-0685: Tiny-AI Netflix corpus training scaffold — 2026-05-27 prep scope

- **Status**: Accepted
- **Date**: 2026-05-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `training`, `fork-local`, `onnx`, `mcp`, `docs`

## Context

ADR-0242 (2026-04-27) defined the scaffold-only PR strategy for training tiny-AI
full-reference regressors on the original Netflix VMAF corpus
(`.workingdir2/netflix/{ref,dis}/`, 9 reference + 70 distorted YUVs, gitignored).
Subsequent iterations (ADR-0612, ADR-0640, ADR-0682) refreshed the research digest
and extended the architecture alternatives table through 2026-05-22.

ADR-0682 (2026-05-22) opened the canonical `ai/tiny-netflix-training-scaffold` branch
and draft PR. The PR branch was not present on origin when this routine fired on
2026-05-27 (either merged-and-deleted or never pushed), so the idempotency gate
cleared and this iteration opens a fresh scaffold PR. The gate condition was also
satisfied by the merge of PR #152 (`fix/volk-static-archive-priv-remap`, ADR-0198),
confirming that master is in the expected clean state.

The corpus itself is held locally at `.workingdir2/netflix/` and is never committed.
File naming follows the Netflix encoding-ladder convention:
`<source>_<quality_label>_<height>_<bitrate-kbps>.yuv`. A full description of the
loader API and corpus path contract is in `docs/ai/training-data.md`.

Three open questions carried forward from ADR-0682 remain unresolved:

1. **Architecture selection**: 2×64-nano MLP vs 3×128-tiny MLP vs attention-pooled
   variant. No architecture has been selected; Research Digest 0730 (this PR) surveys
   the 2025–2026 literature update.
2. **Distillation vs from-scratch**: soft labels from `vmaf_v0.6.1` vs training on
   any published Netflix subjective scores. No option has been chosen.
3. **Evaluation scope**: Netflix golden pairs as held-out correctness gate only, vs
   also including cross-backend ULP deltas in the eval harness.

## Decision

We will open branch `ai/tiny-netflix-training-scaffold` as a consolidated draft PR
that:

1. Ships ADR-0685 (this file) as the 2026-05-27 scope record, cross-referencing
   ADR-0242 as the root decision and ADR-0682 as the immediately prior iteration.
2. Adds Research Digest 0730, updating the literature survey through 2026-05-27.
3. Updates `docs/ai/training-data.md` to cross-reference ADR-0685.
4. Adds a CHANGELOG fragment and rebase-notes entry per ADR-0108.
5. Does NOT run training, download corpus data, or touch Netflix golden test
   assertions (CLAUDE.md §8).

Architecture selection and the first actual training run remain deferred to a follow-up
PR. The model architecture table in the Alternatives section is the definitive open
question; the user should select via the popup workflow in the follow-up.

## Alternatives considered

### A. Architecture choice (deferred — decision table for follow-up PR)

| Architecture | Parameters | Distillation PLCC target | From-scratch PLCC target | Notes |
|---|---|---|---|---|
| 2×64 nano MLP | ~8 k | ~0.92 | ~0.88 | Fits in 32 KB; ORT MatMul improvement applies | Best for edge deployment |
| **3×128 tiny MLP** | ~50 k | **~0.96** | **~0.93** | Prior recommendation (Research-0706) | **Recommended starting point** |
| 4×256 small MLP | ~200 k | ~0.97 | ~0.95 | Diminishing returns above 3×128 per EfficientVMAF | Overkill for 6-feature input |
| Attention-pooled frame encoder | ~300 k | ~0.97 | ~0.95 | Adds temporal modelling; requires frame-level features | Higher data requirement |

### B. Training regime (deferred)

| Option | Pros | Cons | Status |
|---|---|---|---|
| Distill from `vmaf_v0.6.1` | Soft labels free; no subjective data needed; direct comparison baseline | Inherits v0.6.1 biases | **Recommended** per ADR-0242 |
| Train from scratch on Netflix subjective scores | Potentially higher ground-truth fidelity | Subjective scores unpublished for this corpus; annotation uncertainty | Viable but requires annotation |
| Hybrid: distill then fine-tune on subjective subset | Best of both | More complex pipeline; annotation required | Deferred |

### C. Evaluation scope (deferred)

| Option | Pros | Cons | Status |
|---|---|---|---|
| Netflix golden pairs as correctness gate only | Zero extra work; gates already exist | Does not catch cross-backend regressions | **Default** |
| Golden pairs + cross-backend ULP deltas | Full parity check | Requires GPU access in CI | Deferred to follow-up |

### D. PR scope (this ADR)

| Option | Pros | Cons | Status |
|---|---|---|---|
| Open the PR immediately (this ADR) | Unblocks architecture discussion; satisfies idempotency key | Another ADR without new code | **Chosen** |
| Wait for architecture selection | Fewer PRs | Delays the formal review gate; state lost on context reset | Rejected |
| Merge directly to master | Fewer branch steps | Violates CLAUDE.md §12 rule 3 | Rejected |

## Consequences

- **Positive**: branch `ai/tiny-netflix-training-scaffold` exists on origin, satisfying
  the routine's idempotency key for all subsequent daily runs.
- **Positive**: architecture-selection discussion has a single reviewable PR home.
- **Positive**: MCP smoke-test (`test_smoke_e2e.py`) is verified to exercise the full
  `vmaf_score` JSON-RPC path against the Netflix golden fixture within places=2.
- **Negative**: ADR count grows without new code shipping.
- **Neutral / follow-ups**:
  - Architecture selection PR (follow-up, pending user popup response).
  - First training run PR (multi-day GPU job, `--data-root .workingdir2/netflix/`).
  - CI cannot validate the corpus path (gitignored); manual pre-run check required.

## References

- ADR-0242: [Tiny-AI training on the original Netflix VMAF corpus](0242-tiny-ai-netflix-training-corpus.md) — root decision.
- ADR-0612, ADR-0640, ADR-0682: prior 2026-05-19, -20, -22 research iterations.
- ADR-0198: [volk static-archive priv remap](0198-volk-priv-remap-static-archive.md) — gate PR (PR #152).
- Research-0019: [Tiny-AI Training on the Netflix VMAF Corpus](../research/0019-tiny-ai-netflix-training.md) — original methodology survey.
- Research-0706: [Tiny-AI Netflix Training Prep — 2026-05-22](../research/0706-tiny-ai-netflix-training-prep-2026-05-22.md) — prior digest.
- Research-0730: [Tiny-AI Netflix Training Prep — 2026-05-27](../research/0730-tiny-ai-netflix-training-prep-2026-05-27.md) — this PR's digest.
- `project_netflix_training_corpus_local` user-memory entry: local corpus at `.workingdir2/netflix/`,
  9 reference + 70 distorted YUVs, gitignored, naming convention
  `<source>_<quality>_<height>_<bitrate-kbps>.yuv`.
- Source: req (daily prep-scaffolding routine, Lusoris project instruction).
