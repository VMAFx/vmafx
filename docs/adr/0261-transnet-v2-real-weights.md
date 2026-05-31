<!-- markdownlint-disable MD013 MD060 -->
# ADR-0261: TransNet V2 shot-boundary detector — real upstream weights via NTCHW adapter (T6-3a-followup)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `ai`, `dnn`, `feature-extractor`, `wave-1`, `weights-drop`, `shot-detection`, `fork-local`

## Context

[ADR-0223](0223-transnet-v2-shot-detector.md) shipped the TransNet V2
shot-boundary contract — a registered feature extractor `transnet_v2`,
a 100-slot RGB-thumbnail ring buffer in
`core/src/feature/transnet_v2.c`, a smoke-only placeholder ONNX
under `model/tiny/transnet_v2.onnx`, and the smoke test plumbing. The
placeholder was a randomly-initialised tiny MLP matching the declared
I/O shape (`frames` `[1, 100, 3, 27, 48]` luma-broadcast RGB,
`boundary_logits` `[1, 100]`); the registry row carried `smoke: true`
and the doc flagged the missing real weights as backlog item
T6-3a-followup.

Three structural mismatches blocked a verbatim weights drop:

1. **Tensor layout.** Upstream
   [github.com/soCzech/TransNetV2](https://github.com/soCzech/TransNetV2)
   ships a TensorFlow SavedModel whose serving signature takes
   `[batch, frames, height, width, channels]` (NTHWC). The fork's
   C-side extractor (ADR-0223) declared an NTCHW input
   `[1, 100, 3, 27, 48]` to match libvmaf's existing tensor packing
   conventions and the LPIPS / FastDVDnet sibling extractors.

2. **Output multiplicity.** Upstream returns two outputs (`output_1`
   = single-frame logits, `output_2` = auxiliary "many_hot"
   dissolve/fade output, both `[1, 100, 1]`). The fork's C contract
   takes one named output `boundary_logits` shaped `[1, 100]`.

3. **Op-allowlist gaps.** TransNet V2 includes a `ColorHistograms`
   branch that builds per-frame 8x8x8 RGB cubes via
   `tf.math.unsorted_segment_sum` on rank-2 segment IDs. Standard
   ONNX 17 (and tf2onnx 1.17) only lower rank-1 segment ops, so the
   conversion fails on this single node. The rest of the upstream
   graph also exercises six ops not yet on the fork's allowlist
   (`BitShift`, `GatherND`, `Pad`, `Reciprocal`, `ReduceProd`,
   `ScatterND`).

## Decision

We will replace the smoke-only placeholder under
`model/tiny/transnet_v2.onnx` with a **wrapped real-weights export**
that preserves the C contract unchanged. The wrapper is a 4-line
`tf.Module.__call__` that:

- transposes `[1, 100, 3, 27, 48]` (NTCHW) → `[1, 100, 27, 48, 3]`
  (NTHWC) via `tf.transpose(perm=[0, 1, 3, 4, 2])` before invoking
  the upstream serving signature,
- selects only `output_1` (single-frame shot logits) and squeezes
  the trailing singleton dim so the consumer sees `[1, 100]`.

After tf2onnx conversion (with `--continue_on_error` to skip the
unconvertible `UnsortedSegmentSum`), an exporter post-processing pass
**rewrites that single SegmentSum node as an equivalent `ScatterND`
reduction='add' subgraph**:

```text
flat_ids   = Reshape(ids,  [-1, 1])    # [100*1296, 1]
flat_data  = Reshape(data, [-1])       # [100*1296]
zeros      = ConstantOfShape([51200])  # int32
output     = ScatterND(zeros, flat_ids, flat_data, reduction='add')
```

The rewrite is mechanical — no learned parameters change — and
preserves the original semantics:
`output[ids[i,j]] += data[i,j]` for all (i, j).

We extend `core/src/dnn/op_allowlist.c` with the six new standard
ONNX ops the upstream graph relies on (`BitShift`, `GatherND`, `Pad`,
`Reciprocal`, `ReduceProd`, `ScatterND`). Each is deterministic, has
bounded runtime cost, and ships in upstream onnxruntime; widening the
allowlist by these six ops does not introduce control-flow or host
allocation we don't already audit.

The shipped artefacts:

- `model/tiny/transnet_v2.onnx` (~30 MiB; sha256 enforced via the
  registry row);
- `model/tiny/transnet_v2.json` sidecar with full provenance
  (`license: "MIT"`, upstream commit pin, parity-target note);
- `model/tiny/registry.json` row flips `smoke: false`, gains
  `license: "MIT"`, `license_url`, full upstream-commit-pinned
  description.

The exporter `ai/scripts/export_transnet_v2.py` supersedes
`export_transnet_v2_placeholder.py` (kept on disk for reference and
historical reproducibility).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **NTCHW adapter wrapper (chosen)** | Keeps the C contract from ADR-0223 unchanged; the only knowledge of NTHWC lives in the exporter; future weight bumps don't touch C code. | One extra `tf.transpose` op at session entry; fuses cleanly into the first `Conv` so the runtime cost is sub-microsecond. | Picked. The C-side ring-buffer code is load-bearing for two consumers (T6-3a now, T6-3b coming) and re-doing it would invalidate the smoke test that's been green since ADR-0223. |
| Change C contract to NTHWC | Single transpose moved out of the model; simpler graph. | Diverges from sibling tiny-AI extractors (`lpips_sq`, `fastdvdnet_pre`, `mobilesal_placeholder_v0`) which all use channel-first layouts; would force a coupling between the resize loop in `luma_to_thumbnail` and the upstream tensor order that bakes upstream-specific conventions into a fork extractor. | Reject — fork conventions win over per-extractor fidelity. |
| Train a luma-native variant from scratch | Native fit to the C-side luma-only thumbnail; no NTHWC adapter needed. | Hours of training, requires a labelled shot-boundary corpus (ClipShots or RAI Dataset) we don't yet host; no licensing concern with upstream weights to begin with. | Defer to T6-3c follow-up if quality on broadcast-luma RGB inputs proves insufficient. |
| **SegmentSum → ScatterND rewrite (chosen)** | Mechanical, parity-preserving, no learned params involved; produces a graph onnxruntime CPU EP runs natively at opset 17. | Adds `ScatterND` to the op allowlist; one extra ConstantOfShape + Reshape round-trip per inference (negligible: 51200 int32 zeros = 200 KiB scratch). | Picked. The alternative is dropping ColorHistograms entirely, which would change the output and require retraining. |
| Drop ColorHistograms branch entirely | Eliminates the SegmentSum problem; smaller graph. | The published TransNet V2 architecture *uses* the ColorHistograms output as one of the FrameSimilarity inputs to the final classifier; dropping it produces a different network with random output behaviour. | Reject — would break numerical parity with the published model. |
| One-hot + MatMul for SegmentSum | Avoids `ScatterND`. | Materialises a 100×1296×51200-element one-hot tensor (~26 GiB scratch) — runtime-prohibitive. | Reject. |
| **Add 6 ops to allowlist with rationale (chosen)** | Honest engineering — these are standard ONNX ops with bounded behaviour, used by a vetted upstream model. | Allowlist gets larger; future model audits have a slightly bigger set to track. | Picked. The allowlist's purpose is to reject unknown / unsafe ops, not to constrain trained-by-upstream models for which we can audit each op individually. |
| Hand-rewrite each new op as compositions of allowlisted ops | No allowlist change. | `BitShift` and `GatherND` have no clean rewrite to allowlisted ops; `ReduceProd` would have to be a chain of `Mul` reductions which doesn't constant-fold cleanly; rewrite cost is high and the resulting graph is harder to maintain through future upstream bumps. | Reject — the win (no allowlist change) doesn't outweigh the maintenance cost. |
| **Verify TF parity at export time, fail-loud (chosen)** | Catches a broken rewrite immediately; the script asserts `< 1e-4` max-abs-diff over 3 random `[0..255]` inputs (observed: `< 4e-6`). | Adds a TF dependency to the export script. | Picked. The exporter is run rarely (weights bumps), and the live-parity gate is strictly stronger than a static op-allowlist sweep. |

## Consequences

- **Positive**: `transnet_v2` is now a working shot detector. The
  Wave 1 §2.4 row that ADR-0223 half-shipped is fully closed for
  T6-3a; T6-3b (per-shot CRF predictor) can consume real
  `shot_boundary_probability` signal instead of placeholder noise.
  The C-side contract from ADR-0223 is unchanged, so the smoke test
  in `core/test/test_transnet_v2.c` and any downstream pipeline
  built against the placeholder graph keeps working.

- **Negative**: The shipped ONNX is 30 MiB (vs the placeholder's
  125 KiB) — close to but well under the 50 MiB DNN cap in
  `core/src/dnn/model_loader.h`. Loading the model the first time
  in a session adds a one-off ~50 ms ORT-init overhead. The
  ColorHistograms branch is the largest contributor (`ScatterND`
  with 51200-element scratch); leaving it in is a deliberate trade
  for parity vs the published architecture.

- **Neutral / follow-ups**:
  - **T6-3b**: per-shot CRF predictor consuming
    `shot_boundary_probability` per frame.
  - **T6-3c**: native bilinear RGB resize on the C side
    (currently nearest-neighbour luma-broadcast) when a labelled
    shot-boundary corpus becomes available for measuring the
    broadcast-luma quality loss.
  - **Op allowlist invariant**: the six newly-added ops
    (`BitShift`, `GatherND`, `Pad`, `Reciprocal`, `ReduceProd`,
    `ScatterND`) are now load-bearing for `transnet_v2`; removing
    any of them from the allowlist is a model-breakage event.
  - **Upstream commit pin**: the exporter enforces the upstream
    `saved_model.pb` and `variables.data` sha256s. Any future
    weights bump is a deliberate change to
    `UPSTREAM_COMMIT` + the two `UPSTREAM_*_SHA256` constants in
    `ai/scripts/export_transnet_v2.py`.

## References

- Soucek, Lokoc. *TransNet V2: An effective deep network architecture
  for fast shot transition detection*, 2020.
  [arXiv:2008.04838](https://arxiv.org/abs/2008.04838).
- Reference implementation:
  [github.com/soCzech/TransNetV2](https://github.com/soCzech/TransNetV2)
  (MIT-licensed TensorFlow SavedModel).
- Pinned upstream commit:
  [`77498b8e`](https://github.com/soCzech/TransNetV2/commit/77498b8e4a6d61ed7c3d9bd56f4de2b29ab7f4db).
- [ADR-0223](0223-transnet-v2-shot-detector.md) — original
  placeholder-only contract decision (immutable Accepted ADR).
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI 5-point
  per-PR doc bar.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deliverables.
- [ADR-0169](0169-onnx-allowlist-loop-if.md) — op allowlist policy
  (this PR adds six new ops following the same rationale-with-ADR
  precedent).
- [ADR-0215](0215-fastdvdnet-pre-filter.md) /
  [ADR-0255](0255-fastdvdnet-pre-real-weights.md) — sister
  placeholder→real pattern for FastDVDnet (PR #326).
- Source: backlog row T6-3a-followup
  ("convert upstream Soucek & Lokoc 2020 TF checkpoint to ONNX,
  drop under model/tiny/transnet_v2.onnx, flip smoke: false,
  refresh license to MIT").
