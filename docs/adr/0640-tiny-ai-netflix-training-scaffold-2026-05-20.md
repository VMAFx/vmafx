<!-- markdownlint-disable MD013 MD060 -->
# ADR-0640: Tiny-AI training on the original Netflix VMAF training corpus (2026-05-20 scaffold iteration)

- **Status**: Proposed
- **Date**: 2026-05-20
- **Deciders**: Lusoris / Claude (Anthropic)
- **Tags**: `ai`, `docs`, `workspace`, `mcp`

## Context

The fork's tiny-AI surface (`ai/`, `core/src/dnn/`) targets a lightweight
full-reference (FR) regressor that can be distilled from the public
`vmaf_v0.6.1` SVM model or trained directly against subjective scores.  The
user holds the original Netflix VMAF training corpus locally at
`.workingdir2/netflix/` (gitignored, approximately 37 GB):

```text
.workingdir2/netflix/
  ref/    # 9 reference YUVs
  dis/    # 70 distorted YUVs
```

File names follow the Netflix encoding-ladder convention:

```text
<source>_<quality_label>_<height>_<bitrate-kbps>.yuv
```

where `<quality_label> = 0` conventionally identifies the lossless or
near-lossless reference encode.  The corpus was used to train the original
`vmaf_v0.6.1` SVM; reproducing that training pipeline on the fork enables
direct comparisons between the classic SVM and candidate tiny-AI
architectures, and provides a controlled distillation dataset.

Prior scaffold PRs (#153, #418, #759, #920, #1414) and ADRs (ADR-0242,
ADR-0417, ADR-0612) established `docs/ai/training-data.md`,
`mcp-server/vmaf-mcp/tests/test_smoke_e2e.py`, the `--data-root` loader API
in `ai/src/vmaf_train/data/datasets.py`, and the companion research digests
(0019, 0099, 0607).  This ADR is the 2026-05-20 daily scaffold iteration.
It updates the literature survey (Research Digest 0615), extends the
architecture alternatives table with the feature-reweighting option added in
the 2024–2026 IQA distillation literature, and cross-links the updated MCP
smoke test documentation.

The unpublished-Netflix-models search (referenced in user memory) may surface
additional training targets once resolved.  This ADR treats that as an
out-of-scope follow-up.

## Decision

This is a **scaffold-only** PR.  We commit:

1. This ADR (ADR-0640) — updates the architecture alternatives table and
   reinforces the `--data-root` API contract so subsequent training PRs can
   cite a single current source.
2. Research Digest 0615 — a further update of the literature survey covering
   EfficientVMAF (CVPR 2024 workshop), IQA-PyTorch distillation pipelines,
   and ONNX Runtime 1.20 improvements published since Digest 0607 (2026-05-19).
3. An updated cross-reference in `docs/ai/training-data.md` pointing to this
   ADR as the current scaffold iteration.
4. CHANGELOG fragment and rebase-notes entry per ADR-0108.

We do **not** run training, download data, or modify Netflix golden-data
assertions in this PR.  Architecture selection is deferred to the follow-up
training PR, pending user confirmation.

## Alternatives considered

### A. Model architecture

| Option | Description | Pros | Cons | Status |
|--------|-------------|------|------|--------|
| A1 — MLP (2×64, ReLU) | Two-layer MLP on the 6-element VMAF feature vector | Tiny (≈8 KB ONNX), CPU-fast, matches `vmaf_tiny_fr_v1` baseline | Limited capacity for temporal texture | Default candidate |
| A2 — MLP (3×128, ReLU) | Deeper MLP; same input | Higher capacity, still <50 KB | Needs regularisation to avoid overfit on 79-clip corpus | Runner-up |
| A3 — Attention-pooled FR | Per-frame features → self-attention → score | Handles temporal variation better | 10× larger; needs GPU inference for real-time | Deferred |
| A4 — Learned feature-reweighting | 12-param layer over VMAF sub-scores (Madhusudana et al. ICCV 2024) | Interpretable, near-identical compute to SVM | Research-grade; production readiness unproven | Under review |

### B. Training target

| Option | Description | Pros | Cons | Status |
|--------|-------------|------|------|--------|
| B1 — Distill from `vmaf_v0.6.1` | Teacher is the public SVM; student minimises MSE on clip-mean scores | Reproducible without MOS data; aligns with published baseline | Does not exceed teacher ceiling | Recommended |
| B2 — Train from scratch on Netflix MOS | Use published per-clip quality labels from Netflix Blog posts | Could exceed teacher ceiling; directly models human preference | Netflix MOS labels are not fully public; corpus overlap uncertain | Possible follow-up |
| B3 — Hybrid (distill + MOS fine-tune) | Pre-train with B1, fine-tune on whatever MOS labels are available | Best of both if MOS labels exist | Two-stage pipeline; more fragile | Deferred |

### C. Model size

| Option | Target size | Inference FPS (CPU, 1080p) | Status |
|--------|-------------|---------------------------|--------|
| C1 — "nano" < 10 KB ONNX | A1 | >200 FPS | Primary target |
| C2 — "tiny" < 100 KB ONNX | A2 or A4 | 60–200 FPS | Secondary target |
| C3 — "small" < 1 MB ONNX | A3 | <60 FPS | Out of scope for this workstream |

### D. Evaluation scope

| Option | Evaluation dataset | Gate | Status |
|--------|--------------------|------|--------|
| D1 — Netflix golden gate only | `src01_hrc00/01_576x324.yuv` (CPU reference, places=4) | Bit-exact CPU parity | Mandatory |
| D2 — Cross-backend deltas | All enabled backends via `/cross-backend-diff` | ULP ≤ 2 | Mandatory for GPU inference |
| D3 — VMAF v0.6.1 correlation | PLCC/SROCC ≥ 0.97 on Netflix corpus | Quality gate | Added in training PR |
| D4 — External corpus (BVI-DVC, LSVQ) | Existing fork ingestion scripts | Generalisation check | Optional follow-up |

## Consequences

- **Positive**: Provides the current canonical ADR for training PRs to cite;
  refreshes the literature survey; ensures the MCP smoke test and the
  `--data-root` API contract remain documented alongside the training harness.
- **Negative**: Does not train anything; the actual model improvement is
  deferred to a follow-up PR.
- **Neutral / follow-ups**:
  - Architecture selection must be confirmed before the training PR opens.
  - Training run is multi-day and requires GPU access and the local Netflix
    corpus at `.workingdir2/netflix/`.
  - The `--data-root` flag is the mandatory CLI interface; `VMAF_DATA_ROOT`
    env var is the fallback.  Training scripts must not hard-code the corpus
    path.

## References

- ADR-0612: `docs/adr/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md` —
  previous (2026-05-19) scaffold iteration.
- ADR-0242: `docs/adr/0242-tiny-ai-netflix-training-corpus.md` — originating
  architecture and loader-API decision.
- ADR-0042: `docs/adr/0042-tinyai-docs-required-per-pr.md` — doc-substance
  rule for tiny-AI PRs.
- ADR-0108: `docs/adr/0108-deep-dive-deliverables-rule.md` — six deep-dive
  deliverables requirement.
- Research Digest 0615: `docs/research/0615-tiny-ai-netflix-training-2026-05-20.md`
- Research Digest 0607: `docs/research/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md`
- `docs/ai/training-data.md` — corpus path convention, loader API.
- Source: user memory entry `project_netflix_training_corpus_local.md`
  (paraphrase — the corpus lives at `.workingdir2/netflix/`; the Lusoris /
  Claude collaboration ADR documents the decision to scaffold before running).
- Related PRs: #153, #418, #759, #920, #1414.
