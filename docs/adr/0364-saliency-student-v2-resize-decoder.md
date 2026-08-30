# ADR-0364: `saliency_student_v2` — Resize-decoder ablation on the v1 recipe

- **Status**: Accepted (gate passed: v2 IoU 0.7105 ≥ v1 0.6558)
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, mobilesal, saliency, training, fork-local, docs

## Context

[ADR-0286](0286-saliency-student-fork-trained-on-duts.md) shipped
`saliency_student_v1` (2026-05-03) — a fork-trained tiny U-Net
(~113 K params, ConvTranspose stride-2 upsample) trained on DUTS-TR
as the production weights for the C-side `mobilesal` extractor. v1
deliberately avoided `Resize` because the ONNX op-allowlist
([`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c))
did not yet admit it; v1's "Alternatives considered" table records the
trade-off explicitly:

> Use `Resize` for upsampling instead of `ConvTranspose` … `Resize`
> is not on `core/src/dnn/op_allowlist.c` at the time of this PR;
> adding it widens the PR scope into a new C-side audit + scanner
> change in the same diff as the training run. Rejected — `ConvTranspose`
> is on the allowlist already and produces a numerically equivalent
> stride-2 upsample for our purposes.

[ADR-0258](0258-onnx-allowlist-resize.md) (accepted 2026-05-03) closed
that gap by widening the allowlist to admit `Resize`. The "Resize +
Conv" pattern — bilinear `F.interpolate` followed by a 3×3 `Conv2d` —
is the de-facto standard U-Net upsampling shape in the broader
image-segmentation literature; v2 is the natural ablation that swaps
in this pattern while holding everything else (encoder backbone,
channel widths, skip connections, loss, optimizer, schedule,
augmentation, seed) identical to v1, so the ablation is clean.

The training corpus, ship-gate (≥ v1's 0.6558 held-out IoU per
ADR-0303-shape gate, no test-weakening), and I/O contract stay
unchanged.

## Decision

We will train and ship `saliency_student_v2` — a fork-trained tiny
U-Net (~124 K parameters, ONNX opset 17, BSD-3-Clause-Plus-Patent) as
a new `model/tiny/saliency_student_v2.onnx` registry entry. The model
is trained from scratch on DUTS-TR with `ai/scripts/train_saliency_student_v2.py`
(forked from v1's trainer; only the decoder upsampling path changed)
and exposes the same `input` / `saliency_map` tensor contract as v1
and `mobilesal_placeholder_v0`.

`saliency_student_v1` stays as the production weights for the
`mobilesal` extractor; v2 ships as a parallel artefact under
`model/tiny/`. Promotion to production is a future PR after empirical
validation in real ROI encodes — this PR establishes the pattern and
the artefact, not the production flip.

The held-out IoU gate from ADR-0303-shape is non-negotiable per
`feedback_no_test_weakening`: v2's 5 % DUTS-TR validation-fold IoU
must match or beat v1's 0.6558. If it does not, the model ships with
`Status: Proposed (gate-failed: v2 IoU [%.4f] < v1 [0.6558])` and the
production-flip PR is blocked.

The op-allowlist scope: ADR-0258 admits `Resize` op-type-only and
explicitly does **not** enforce attributes. v2 exports with
`mode='linear'`, `coordinate_transformation_mode='half_pixel'`,
`antialias=0` (the bilinear `F.interpolate(align_corners=False)`
default produced by the legacy TorchScript exporter at opset 17).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Resize + 3×3 Conv decoder (this ADR)** | Standard U-Net upsampling pattern from segmentation literature; clean ablation against v1 (one architectural change); exercises the new ADR-0258 allowlist affordance; learnable filter after a fixed-kernel resample is the strictly more expressive shape | ~+11 K params vs v1 (3×3 vs 2×2 kernel); marginally slower forward (one extra resample-then-conv vs a fused transposed-conv) | **Chosen** — establishes the pattern post-ADR-0258 and is the directly comparable ablation |
| Resize + 1×1 Conv decoder | Strict parameter reduction vs v1 (1×1 has fewer weights than ConvT 2×2) | 1×1 cannot mix neighbouring spatial info after the resample; reduces decoder capacity below v1; not a faithful "upgrade v1" ablation | Rejected — the brief asked for the standard pattern, not a parameter-minimised variant |
| Bigger student + multi-dataset corpus | Higher absolute IoU; closer to upstream u2netp territory | Conflates two changes (architecture + corpus); v2 stops being a clean ablation; deferred per ADR-0286's `saliency_student_v2` follow-up plan | Rejected here — saved for `saliency_student_v3` |
| Stay on ConvTranspose (no v2) | Zero engineering | ADR-0258 is an unused affordance; the segmentation-literature pattern stays unverified on this fork's corpus | Rejected — the affordance was opened specifically to unblock this kind of pattern |
| Replace v1 in the registry instead of shipping v2 in parallel | Single canonical artefact | Untested empirically in real ROI encodes; user direction is "v2 ships as a parallel artifact; promotion to production is a future PR after empirical validation" | Rejected — production flip is a separate PR |

## Consequences

- **Positive**:
  - Exercises the ADR-0258 `Resize` affordance with a real, in-tree
    consumer — proves the allowlist change end-to-end (ORT loads,
    parity check passes, op-scanner accepts).
  - Establishes a clean, single-variable ablation on the v1 recipe;
    future architectural choices (kernel size, multi-dataset corpus)
    can stack on this baseline.
  - Adds `Resize` to the in-tree fork-trained ONNX surface so any
    future tiny-AI model that wants the pattern has a precedent
    (`docs/ai/models/saliency_student_v2.md`).
- **Negative**:
  - Adds a second saliency student to the model directory and the
    registry — operators must decide which to consume. Mitigation:
    `saliency_student_v1` stays the documented production weights
    until v2 passes ROI-encode validation in a follow-up PR.
  - DUTS-TR is still 271 MB and not redistributed in-tree;
    reproducing v2 has the same dataset-fetch step as v1.
- **Neutral / follow-ups**:
  - **Production flip**: a follow-up PR will A/B v1 vs v2 in real
    ROI-encode pipelines and either promote v2 or rule it
    not-affecting-quality. Filed as a backlog row.
  - **`saliency_student_v3`**: the multi-dataset (DUTS-TR ∪ MSRA10K
    ∪ HKU-IS) and ~300 K-param backlog item from ADR-0286 stays
    open and is independent of v2.
  - **External evaluation**: the DUTS-TE / ECSSD held-out evaluation
    follow-up from ADR-0286 should run against both v1 and v2 once
    the harness lands.

## References

- [ADR-0258](0258-onnx-allowlist-resize.md) — admits `Resize` op-type
  to the allowlist; this ADR is the first fork-local consumer.
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) — v1
  decision record; this ADR holds v2's recipe identical to v1's
  except for the upsampling path.
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — the
  no-test-weakening production-flip gate shape this ADR follows.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this PR satisfies (ships
  `docs/ai/models/saliency_student_v2.md`).
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables checklist.
- DUTS-TR dataset: Wang, Lu, Wang, Feng, Wang, Yin, Ruan, "Learning
  to Detect Salient Objects with Image-Level Supervision", CVPR 2017.
  Project page: <http://saliencydetection.net/duts/>. Direct URL:
  `https://saliencydetection.net/duts/download/DUTS-TR.zip`.
  Distribution: free for academic and research purposes (per the
  project page). Same provenance as v1.
- Source: paraphrased — task brief directive "train
  `saliency_student_v2.onnx` using the `Resize` op now allowed in
  the op-allowlist (per ADR-0258), v2 uses Resize (linear,
  half_pixel) for the upsampling path."
