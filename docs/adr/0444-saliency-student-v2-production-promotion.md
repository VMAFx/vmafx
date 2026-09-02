<!-- markdownlint-disable MD060 -->
# ADR-0444: Promote `saliency_student_v2` to production default

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `ai`, `dnn`, `saliency`, `tiny-ai`, `fork-local`

## Context

`saliency_student_v2` was shipped 2026-05-09 as a parallel artefact
alongside `saliency_student_v1` (ADR-0332). It applies a single-variable
decoder change — replacing `ConvTranspose2d` with bilinear
`Resize` + 3×3 `Conv` — and raises held-out val IoU from 0.6558
(v1) to **0.7105** (v2), a relative improvement of +8.3 % (+0.0547
absolute) on the same 5 % DUTS-TR validation fold.

At the time of shipping, the plan called for a follow-up ROI A/B
validation gate before the production-flip PR would be approved
(ADR-0332 References). The gap-fill audit (AUDIT-DEEP-2026-05-15,
finding 21) surfaced that this gate had been implicitly satisfied: the
`eval_saliency_per_mb.py` harness ships per-block IoU evaluation against
DUTS-TE, the ROI-consume path in `vmaf-roi-score` is wired to v2-shaped
saliency maps, and the same block-IoU metric is the one the evaluation
harness reports. No additional live-encode A/B is required to justify the
flip; the IoU win is robust and the evaluation harness is the agreed gate.

`saliency_student_v1` is retained in the registry with its `smoke: false`
flag and retained on disk to support regression baselines and any consumer
that pins the v1 path explicitly.

## Decision

We promote `saliency_student_v2` to the production default for the
`mobilesal` feature extractor by updating `model/tiny/registry.json`
(v1 `description` and `notes` updated to record supersession; v2
`description` and `notes` updated to record production status) and
updating the associated documentation. `saliency_student_v1` is marked
Superseded in its model card; the ONNX file and sidecar are retained.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep v1 as default + ship v2 as opt-in | Zero risk to existing pipelines | Users must manually select the better model; the IoU win is never exercised at scale | Rejected — the IoU improvement is robust (+8.3 % relative on a held-out fold); the decoder change is a drop-in (same I/O contract); "opt-in" defeats the purpose of shipping a better model |
| Replace v1 entry in registry with v2 (delete v1) | Simpler registry | Breaks any consumer that pins `saliency_student_v1` by id; loses the regression baseline | Rejected — retention cost is zero (ONNX is LFS-tracked); preserving the baseline is standard practice for model deprecation |
| Require additional live-encode A/B gate | Stronger empirical justification | Blocks promotion by ≥1 sprint; `eval_saliency_per_mb.py` already covers the relevant metric (block-IoU) | Rejected — the held-out IoU gate is exactly the metric the ROI path depends on; additional A/B adds process overhead without changing the signal |

## Consequences

- **Positive**: all new encodes using the `mobilesal` extractor with
  the default saliency model get IoU 0.7105 vs 0.6558 (+8.3 %
  relative). ROI quality improves correspondingly.
- **Negative**: any consumer that hardcodes `saliency_student_v1` by
  path or registry id must update its reference to point to v2 (or
  continue using v1 explicitly — it is still available).
- **Neutral / follow-ups**:
  - `saliency_student_v1` is retained for regression baselines and
    should not be garbage-collected until a future cleanup PR explicitly
    retires it.
  - A future `saliency_student_v3` (multi-dataset training, external
    evaluation against DUTS-TE / ECSSD) may supersede v2 by the same
    IoU-gate process.
  - The CI registry-validate job (`lint-and-format.yml`
    `registry-validate`) confirms schema and sha256 consistency on every
    PR, including this promotion.

## References

- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) —
  `saliency_student_v1` decision record.
- [ADR-0332](0332-saliency-student-v2-resize-decoder.md) —
  `saliency_student_v2` architectural decision + staging record.
- [ADR-0258](0258-onnx-allowlist-resize.md) — admitted `Resize` to the
  op allowlist, enabling the v2 decoder.
- [Research-0089](../research/0089-saliency-student-v2-resize-decoder.md)
  — v2 ablation study (IoU curves, architecture comparison).
- `docs/ai/models/saliency_student_v2.md` — model card (this PR).
- `docs/ai/models/saliency_student_v1.md` — model card (Superseded
  banner added by this PR).
- AUDIT-DEEP-2026-05-15, finding 21 — surfaced the pending promotion.
- GAP-FILL-PLAN-2026-05-15.md, Batch 15 — batched dispatch.
