<!-- markdownlint-disable MD013 MD060 -->
# ADR-0223 TransNet V2 shot-boundary detector — 100-frame window, placeholder weights

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `ai`, `dnn`, `feature-extractor`, `wave-1`, `shot-detection`, `fork-local`

## Context

Wave 1 of the tiny-AI roadmap (`docs/ai/roadmap.md` §2.4) splits content-
adaptive encoding into a two-step pipeline: first a shot-boundary
detector emits per-frame shot-change scores, then a per-shot CRF
predictor (small CNN/MLP) consumes the resulting shot intervals and
produces a CRF target per shot. The published TransNet V2 architecture
(Soucek & Lokoc 2020, ~1M parameters, MIT-licensed) is the reference
shot-boundary detector: it consumes a 100-frame sliding window of small
27x48 RGB thumbnails and emits one shot-change probability per frame.

Backlog row T6-3a (post-2026-04-28 audit) splits the published pipeline
into two PRs: this PR ships only the shot-boundary feature extractor;
the per-shot CRF predictor is **T6-3b**, a follow-up that consumes the
per-frame probabilities through the existing feature collector.

We need three things in one PR: (1) a working contract on the libvmaf
side that the eventual per-shot CRF predictor can plug into,
(2) an ONNX checkpoint that the contract can load, and (3) the standard
tiny-AI deliverables (registry row, sidecar JSON, docs, ADR, smoke
test). Real TransNet V2 weights from
[github.com/soCzech/TransNetV2](https://github.com/soCzech/TransNetV2)
ship as a TensorFlow checkpoint that needs `tf2onnx` plus a manual
graph cleanup before the fork's strict op allowlist will accept it —
out-of-scope for one PR. So this PR ships the contract + a smoke-only
placeholder ONNX, with the real weights drop tracked as
**T6-3a-followup**.

## Decision

We will ship TransNet V2 as a registered feature extractor
`transnet_v2` in `core/src/feature/transnet_v2.c`, backed by an
ONNX model whose I/O contract is

```text
input  "frames"          : float32 [1, 100, 3, 27, 48]
                            (100-frame window, RGB, 27x48 thumbnails)
output "boundary_logits" : float32 [1, 100]
                            (per-frame shot-boundary logits)
```

The extractor maintains an internal 100-slot ring buffer of pre-resized
RGB thumbnails (luma broadcast across the 3 RGB channels for the
placeholder graph; T6-3a-followup switches to true RGB decode when
real upstream weights land), gathers the current window into the input
tensor, runs `vmaf_dnn_session_run`, and emits two per-frame features:

- **`shot_boundary_probability`** — `sigmoid(logit)` for the current
  frame, taken from the most-recent slot of the output tensor.
- **`shot_boundary`** — binary 0/1 thresholded at 0.5 against the
  probability. Downstream consumers that want to bucket the timeline
  into shot intervals (T6-3b per-shot CRF predictor, FFmpeg shot-cut
  filter) bind to this exact name.

Initial PR ships a smoke-only placeholder ONNX
(`model/tiny/transnet_v2.onnx`, ~125 KB, randomly-initialised tiny MLP
with the correct input/output shape); T6-3a-followup swaps the weights
against a TF→ONNX-converted Soucek & Lokoc 2020 checkpoint.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Placeholder ONNX (chosen)** | Unblocks contract + integration in one PR; T6-3a-followup is a weights-only change. | Not a real shot detector yet; downstream consumers must wait for T6-3a-followup to derive real boundary signal. | Picked. Smoke-only is clearly labelled in the registry (`smoke: true`) and the sidecar JSON. |
| Real upstream weights (soCzech) | Working detector day one. | Upstream ships TensorFlow checkpoint, not ONNX. `tf2onnx` conversion needs manual graph-cleanup + op-allowlist verification we'd want in a separate PR with its own validation. | Deferred to T6-3a-followup; bundle the conversion + validation with the weights drop. |
| Train a fresh checkpoint locally | No license/sourcing concern; full control over architecture. | Hours of training + dataset prep (RAI dataset, ClipShots); out-of-scope for a single PR. | Not done in this PR; could be revisited if upstream conversion proves unworkable. |
| **100-frame window (chosen)** | Matches published TransNet V2 architecture; downstream consumers see the canonical contract. | Requires a 100-slot ring buffer; ~5 MB scratch for default 27x48 thumbnails (cheap). 50-frame warm-up before the detector is fully informed. | Picked. Ring-buffer cost is trivial and matching the upstream contract is the whole point of this PR. |
| Sliding incremental window (e.g. 16 frames stride 1) | Lower memory; lower per-frame compute. | Diverges from the published TransNet V2 contract; downstream CRF predictor in T6-3b would have to learn against a non-standard signal; placeholder/real weights would not be drop-in interchangeable. | Stick with 100-frame full-window to match the paper. |
| **T6-3a vs T6-3b boundary at probability emit (chosen)** | Per-frame probability is the canonical TransNet V2 output. T6-3b's per-shot CRF predictor consumes it through the existing feature collector — no new shared state, no new C-side coupling. | Two PRs instead of one; an interim period where consumers see boundaries but no CRF target. | Picked. Splitting at the per-frame probability boundary keeps each PR scoped to one model and one decision. |
| Combine T6-3a + T6-3b in one PR | Single PR delivers the full content-adaptive encoding loop. | Two ML models in one PR ≈ 2× the review surface; per-shot CRF predictor depends on a shot-list aggregation step that pulls in additional design (shot-merge thresholds, min-shot-length filtering). | Reject — separate PRs per backlog rows T6-3a / T6-3b. |
| Per-frame probability output (chosen) | Matches TransNet V2's published behaviour; lets T6-3b implement its own shot-merge / min-length aggregation. | Per-frame probability isn't directly actionable — caller has to threshold + group. | Picked. Emit the raw probability **and** a default-thresholded binary flag so naive consumers have a ready-to-use signal too. |
| Per-shot bucket output (post-aggregation in C extractor) | Single-shot metadata is what the encoder ultimately consumes. | Aggregation strategy (threshold, hysteresis, min-shot-length) is itself a design decision we'd want to defer to T6-3b. C extractors emit one feature per index; per-shot output would have to fudge per-frame indices anyway. | Reject — keep aggregation on the consumer side. |
| Threshold flag inside extractor (default 0.5) | Cheap convenience for consumers that just want "is this a cut?"; threshold visible in code. | Hard-codes a threshold the user might want to tune. | Emit both — `shot_boundary_probability` for tunable consumers, `shot_boundary` for ready-to-use. The threshold is a single named constant so adding a feature option is a follow-up if someone needs it. |
| ORT EP selection (force CPU vs auto) | Predictable inference path; no surprise device fallback. | Defeats the multi-EP design that the rest of the tiny-AI surface inherits from `vmaf_dnn_session_open`. | Use `VMAF_DNN_DEVICE_AUTO` (the default) — same as `lpips_sq` and `fastdvdnet_pre`. |

## Consequences

- **Positive**: Wave 1 §2.4 row half-shipped; T6-3b per-shot CRF
  predictor is now a "consume `shot_boundary_probability` from the
  feature collector" PR. The 100-frame ring + head-clamp behaviour is
  fixed in C and exercised by the unit test shape contract. Two
  feature names (`shot_boundary_probability` + `shot_boundary`) are
  load-bearing names that downstream code binds to.
- **Negative**: The shipped ONNX is not a real shot detector. Anyone
  running `vmaf --feature transnet_v2` against the placeholder will
  see noisy mid-range probabilities (sigmoid of small random logits)
  with no relationship to real shot boundaries. The registry entry's
  `smoke: true` flag and the sidecar's `notes` field both call this
  out; the user-facing doc spells out the path to T6-3a-followup.
- **Neutral / follow-ups**:
  - **T6-3a-followup**: convert the upstream Soucek & Lokoc 2020
    TensorFlow checkpoint to ONNX, drop it under
    `model/tiny/transnet_v2.onnx`, flip `smoke: false` in the
    registry, refresh the license field to `MIT` (upstream license).
  - **T6-3b**: per-shot CRF predictor consuming
    `shot_boundary_probability` per frame, plus shot-merge /
    min-length aggregation logic.
  - **AGENTS.md invariant**: the 100-frame-window contract +
    ring-buffer head-clamp behaviour + dual feature-name surface are
    now load-bearing; any rebase that touches `transnet_v2.c` must
    preserve all three.
  - **Op allowlist**: the placeholder graph uses only ops already in
    [`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c)
    (`Reshape`, `MatMul`, `Add`, `Relu`); the real upstream weights
    drop will need to verify the allowlist still covers TransNet V2's
    upstream graph (DDCNN-style 3D dilated convolutions) before
    flipping the smoke flag.

## References

- Soucek, Lokoc, *TransNet V2: An effective deep network architecture for fast shot transition detection*, 2020. [arXiv:2008.04838](https://arxiv.org/abs/2008.04838).
- Reference implementation: [github.com/soCzech/TransNetV2](https://github.com/soCzech/TransNetV2) (MIT-licensed TensorFlow checkpoint).
- [`docs/ai/roadmap.md` §2.4](../ai/roadmap.md) — Wave 1 row that schedules this work.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI 5-point per-PR doc bar.
- [ADR-0107](0107-tinyai-wave1-scope-expansion.md) — Wave 1 scope.
- [ADR-0168](0168-tinyai-konvid-baselines.md) — baseline tiny-AI checkpoint shape we mirror.
- [ADR-0215](0215-fastdvdnet-pre-filter.md) — sister T6-7 placeholder pattern (5-frame ring buffer; this PR uses the same precedent at 100 frames).
- Source: `req` — backlog row T6-3a in `.workingdir2/BACKLOG.md` ("TransNet V2 shot detector ~1M params (T6-3a). Pairs with T6-3b per-shot CRF predictor.").
