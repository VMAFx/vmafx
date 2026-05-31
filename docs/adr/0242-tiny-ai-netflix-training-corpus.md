<!-- markdownlint-disable MD060 -->
# ADR-0242: Tiny-AI training on the original Netflix VMAF training corpus

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `training`, `fork-local`, `onnx`, `docs`

## Context

The fork's tiny-AI surface (`ai/`, `core/src/dnn/`) ships small ONNX
full-reference (FR) regressors trained from libvmaf feature vectors. Until
now those models were trained on the five canonical public datasets documented
in `docs/ai/training.md` (NFLX Public, KoNViD-1k, LIVE-VQC, YouTube-UGC,
BVI-DVC). A separate, unpublished copy of the original Netflix VMAF training
corpus — 9 reference YUVs and 70 distorted YUVs produced under the encoding
ladder used to build `vmaf_v0.6.1` — is available on the user's local machine
at `.workingdir2/netflix/{ref,dis}/`. That path is gitignored; the corpus is
never committed.

The corpus naming pattern follows the Netflix encoding-ladder convention:

```text
<source>_<quality_label>_<height>_<bitrate-kbps>.yuv
```

Training directly on these pairs gives tiny-AI models the best chance of
matching or exceeding `vmaf_v0.6.1` accuracy on Netflix-internal quality
ladders — a goal surfaced in the unpublished-Netflix-models research thread
(see user memory entry `project_netflix_training_corpus_local.md`). The
relationship between this corpus and the three Netflix golden CPU reference
pairs preserved in `python/test/resource/yuv/` (see `CLAUDE.md §8`) must be
explicit: the golden pairs are a held-out correctness gate and are never used
as training data.

This ADR is a scaffold-only decision. The actual training run requires GPU
access, is estimated at multiple days of wall-clock time per architecture
sweep, and depends on forthcoming decisions about model architecture and
distillation strategy. This PR defers those decisions deliberately.

## Decision

We will merge a scaffold-only PR that:

1. Documents the corpus path convention, the `--data-root` loader API, and
   the recommended evaluation harness in `docs/ai/training-data.md`.
2. Records the architecture-choice space and distillation policy in this ADR
   (§ Alternatives considered) without picking any option yet.
3. Adds an MCP end-to-end smoke test (`mcp-server/vmaf-mcp/tests/test_smoke_e2e.py`)
   that exercises the JSON-RPC `vmaf_score` tool against the smallest Netflix
   golden fixture, giving the user one-command verification that the MCP server
   is wired correctly before they attach Claude Code's MCP client to it.
4. Does NOT run training, download data, or touch Netflix golden test assertions.

Actual training, architecture selection, and hyperparameter choices will land
in a follow-up PR once the user has reviewed the alternatives table and answered
any popup questions from the training agent.

## Alternatives considered

### A. Model architecture

| Option | Pros | Cons | Status |
|---|---|---|---|
| 2-layer MLP on libvmaf feature vectors (current `fr_tiny_v1` baseline) | Fast to train and evaluate; deterministic; interpretable; no new deps | Accuracy ceiling bounded by the hand-crafted features; no spatial sensitivity | **Default starting point** |
| 4-layer MLP with batch-norm | Higher capacity; still lightweight for ONNX export | Overfit risk on 70-pair corpus; need careful regularisation | Viable; evaluate in sweep |
| 1-D CNN over temporal feature sequences | Captures motion/temporal quality trends | Much larger; training data sparse for temporal modelling on 70 pairs | Defer to Phase 4 |
| Transformer on feature tokens | SOTA on NR tasks; flexible attention | Overkill for FR on 70 pairs; prohibitive training time without pre-training | Deferred |

### B. Distillation vs from-scratch

| Option | Pros | Cons | Status |
|---|---|---|---|
| Distill from `vmaf_v0.6.1` (soft-label regression) | Tiny-model inherits `vmaf_v0.6.1` score distribution without needing raw MOS | Output is bounded by teacher; systematic teacher errors are inherited | **Recommended starting point** |
| Train from scratch on subjective scores Netflix published (ACM MM 2016 appendix) | Ground truth independent of teacher; potential to exceed `vmaf_v0.6.1` | Published MOS for only a subset of pairs; high variance on 70-pair corpus | Viable; run in parallel sweep |
| Fine-tune an existing ONNX checkpoint | Fast convergence; stable initialisation | Risk of catastrophic forgetting; checkpoint may not exist for the right opset | Deferred |

### C. Model size

| Option | Target params | Inference budget (CPU, 1080p) | Notes |
|---|---|---|---|
| Micro (≤ 4 KB ONNX) | < 1 K | < 2 ms | Fits embedded / Wasm targets |
| Small (≤ 64 KB ONNX) | 4 K – 16 K | 2–10 ms | Current `fr_tiny_v1` range |
| Medium (≤ 512 KB ONNX) | 16 K – 128 K | 10–50 ms | Headroom for spatial features |

### D. Evaluation scope

| Option | Pros | Cons | Status |
|---|---|---|---|
| Netflix golden CPU pairs only (3 pairs, `python/test/`) | Locked CI gate; regression-proof | Tiny sample; overfits to golden distribution | Required gate, not sole criterion |
| Cross-backend ULP delta (`/cross-backend-diff`) | Verifies numerical parity GPU↔CPU | Doesn't measure perceptual accuracy | Required gate for GPU paths |
| Both golden + cross-backend + PLCC/SROCC on held-out split | Comprehensive | Most expensive to run each PR | **Recommended for release gate** |

## Consequences

- **Positive**: the training pipeline is clearly specified and ready to invoke
  interactively once the user confirms architecture choices; the MCP smoke
  test immediately catches MCP-server regressions; the data-path convention
  prevents accidental corpus commits.
- **Negative**: actual training runs are multi-day and GPU-bound; the corpus
  is local-only, so CI cannot gate on training correctness (only on the smoke
  test against the golden fixture).
- **Neutral / follow-ups**:
  - Follow-up PR to pick architecture, run training, export ONNX, register
    model, update `docs/ai/models/`.
  - ADR-0042 doc-substance rule applies: the follow-up PR must ship updated
    docs alongside the trained artefact.
  - The `--data-root` flag must be added to `vmaf-train extract-features`
    (currently it reads from `${VMAF_DATA_ROOT}` env var; an explicit CLI
    flag is cleaner for interactive use).

## References

- User memory entry `project_netflix_training_corpus_local.md` (paraphrased;
  Lusoris/Claude collaboration record — not committed).
- Li, Z. et al. "Toward a Practical Perceptual Video Quality Metric." Netflix
  Tech Blog, 2016. (originating methodology for `vmaf_v0.6.1`.)
- Netflix Tech Blog: "VMAF: The Journey Continues" (2018, 2020).
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI doc-substance rule.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive deliverables.
- [Research digest 0019](../research/0019-tiny-ai-netflix-training.md).
- Related PR: this scaffold PR (feat(ai): tiny-AI training scaffold + MCP
  smoke test (Netflix corpus prep)).
- Source: `req` (direct user instruction in daily prep-scaffolding routine).
