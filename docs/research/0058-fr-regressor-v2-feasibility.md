# Research-0058: FR regressor v2 (codec-aware) feasibility

- **Date**: 2026-05-03
- **Status**: Scaffold (training deferred until vmaf-tune Phase A
  produces a real corpus)
- **Companion ADR**: [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md)
- **Builds on**: [ADR-0235](../adr/0235-codec-aware-fr-regressor.md)
  (codec-aware decision), [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
  (vmaf-tune Phase A), [ADR-0249](../adr/0249-fr-regressor-v1.md)
  (v1 baseline)

## Question

Now that vmaf-tune Phase A (PR #329) ships the corpus tooling that
emits one JSONL row per `(source, encoder, preset, crf)` cell, how
should `fr_regressor_v2` consume that corpus, what input shape should
it pin, and what's the realistic accuracy ceiling vs the v1 baseline?

## Why codec-aware at all

The v1 regressor (ADR-0249) takes the canonical-6 libvmaf feature
vector — `adm2`, `vif_scale0..3`, `motion2` — and predicts a VMAF
teacher score. The teacher VMAF score is itself codec-blind: the same
feature vector from an x264 encode and a libvvenc encode lands at the
same point on the 6-D feature manifold even though the underlying
distortion signature is structurally different (block edges vs CTU
deblocking vs DCT ringing vs in-loop restoration filters).

That's a feature for v1 — it stays a faithful tiny mirror of VMAF —
but it caps cross-codec generalisation. The Bristol VI-Lab review
§5.3 (cited verbatim in ADR-0235) reports a 1–3 PLCC-point lift on
multi-codec corpora when a perceptual regressor is conditioned on
codec id; Bampis 2018 (ST-VMAF) and Zhang 2021 ("Enhancing VMAF")
report similar deltas in their per-codec ablations.

v2 inherits v1's 6 canonical features and adds a codec block so it
can learn the **residual** codec × preset × CRF effect on top of
what the canonical-6 already captures. The canonical-6 still carries
the bulk of the signal; the codec block is a corrective bias term.

## Why MLP, not GBDT

The fork's runtime DNN surface (`core/src/dnn/`) is built around
ONNX Runtime with a strict op allowlist (see
`ai/src/vmaf_train/op_allowlist.py`). GBDT exports (LightGBM /
XGBoost) require either an in-tree forest evaluator or `Gather`-heavy
ONNX trees that don't fit the existing pattern. Sticking with the
MLP pattern means:

- Same export path as v1 (`export_to_onnx` with opset 17, dynamic
  batch axis, op-allowlist check).
- Same INT8 PTQ tooling applies if v2 needs quantisation later.
- The two-input session contract reuses the LPIPS-Sq precedent
  (ADR-0040 / ADR-0041) — `vmaf_dnn_session_run` already supports it.
- Hidden width 16 stays under v1's 64 because the codec block is
  low-entropy (8 dims, mostly one-hot) and a wider MLP would just
  memorise the training corpus.

The v1 PLCC ceiling on Netflix Public was 0.9977 ± 0.0025 mean LOSO;
the codec-aware lift in literature is 1–3 PLCC points on multi-codec
corpora, so a v2 ceiling of ~0.999 in-domain is plausible. The real
question is out-of-domain — see open question §4 below.

## Input shape decision

| Layout | Pros | Cons | Picked? |
| --- | --- | --- | --- |
| **6 canonical + 8-D codec block (one-hot encoder + preset_norm + crf_norm)** | Two-input ONNX with named tensors; preset_norm carries continuous speed-quality trade-off cross-encoder; CRF normalised by union upper bound | Slight waste from 6-way one-hot; preset ordinal lossy for libsvtav1 (0..13 squashed to 0..9) | Yes |
| Concatenated 14-D single input | Single-tensor ONNX, simpler libvmaf wiring | Loses semantic boundary between feature and codec dims; can't gate codec block off at inference (e.g. for codec-blind fallback) | No — two-input matches LPIPS-Sq pattern, future-proofs for codec-blind eval |
| Continuous learned embedding `nn.Embedding(N_codecs, d_emb)` | Compact (4–8 dims), graceful for unknown codecs | Adds `Gather` op to allowlist; no measurable accuracy delta at this corpus scale per ADR-0235's analysis | No — adds runtime complexity for no measurable gain |
| 6 + 6 (encoder one-hot only, drop preset/crf) | Simplest extension of v1 | Throws away the rate-distortion signal; preset and CRF carry the bulk of the within-codec quality variation | No |

Picked **6 + 8** because it preserves a clean train-once / infer-many
contract: at inference time the codec block can be replaced with a
"codec-blind fallback" (all-zeros + 0.5 / 0.5) and the model degrades
gracefully to a v1-like estimate.

## Smoke validation

The smoke path generates 100 synthetic rows whose VMAF target is a
deterministic function of CRF + per-encoder offset + Gaussian noise.
This validates the pipeline end-to-end (JSONL ingest → 9-D
materialisation → MLP train → ONNX export → op-allowlist check →
torch-vs-ORT roundtrip) without burning hours on a real Phase A
corpus run. Smoke output:

```text
$ python ai/scripts/train_fr_regressor_v2.py --smoke
[fr-v2] SMOKE mode — synthesising 100 fake corpus rows
[fr-v2] materialising 100 rows -> 9-D feature space
[fr-v2] shapes: canon=(100, 6) codec=(100, 8) y=(100,)
  epoch 1/1 loss=4737.44
[fr-v2] training done in 1.7s
[fr-v2] in-sample: PLCC=-0.4072 SROCC=-0.4276 RMSE=68.548
[fr-v2] shipped: model/tiny/fr_regressor_v2.onnx
```

Negative PLCC after a single epoch is expected — the goal of `--smoke`
is **pipeline validation, not accuracy**. The output ONNX is flagged
`smoke: true` in the registry and is excluded from the quality-metric
harness exactly like the existing `smoke_v0.onnx` precedent.

## Open question: corpus diversity for production v2

This is the load-bearing risk and the reason v2 ships as a scaffold
rather than a trained model.

The vmaf-tune Phase A schema is a **grid sweep** over
`(source, encoder, preset, crf)` cells. A grid corpus has uniform
preset/CRF coverage but **synthetic source diversity** — one source
contributes 5 encoders × 5 presets × 6 CRFs = 150 rows of the *same*
underlying perceptual content. If the production v2 trains on a
small-source-count grid corpus, the MLP will overfit to the few
sources and the PLCC lift will be illusory.

Plausible corpus regimes:

1. **Phase A grid corpus, narrow source set (5–10 refs)** — fast to
   produce (hours, not days), but the MLP will memorise per-source
   feature/codec correlations. Likely PLCC lift on held-out cells of
   *seen* sources; meaningless lift on held-out *unseen* sources.
2. **Phase A grid corpus, broad source set (50+ refs)** —
   acceptable diversity; closes most of the source-overfitting risk;
   compute cost is days-to-weeks per encoder.
3. **Real-world video set with naturally-distributed encodes** —
   highest signal but lowest control; corpus is whatever the
   collection happens to ship (e.g. UGC streaming traces). v2 doesn't
   currently have an ingest pipeline for this regime.

Recommendation: defer the production v2 training run until either
(a) the Netflix Public corpus + BVI-CC (ADR-0241) source pool plus a
Phase A grid produces a 50+ source / 5+ encoder corpus, or
(b) the Phase A schema gains per-frame feature emission so the
training pipeline doesn't have to assume one synthetic feature row
per (source, encoder, preset, crf) cell.

The scaffold registers v2 as `smoke: true` so the production training
run is a follow-up PR with a clear gate: ship the trained checkpoint
only if held-out (LOSO over sources) PLCC clears v1's 0.95 ship
threshold *and* shows a ≥0.005 lift on multi-codec splits per the
ADR-0235 ship gate.

## References

- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — codec-aware
  FR regressor decision; cites Bristol VI-Lab review §5.3, Bampis
  2018 (ST-VMAF), Zhang 2021 ("Enhancing VMAF").
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) —
  vmaf-tune Phase A schema and roadmap.
- [ADR-0249](../adr/0249-fr-regressor-v1.md) — v1 baseline.
- [Research-0044](0044-quality-aware-encode-automation.md) —
  vmaf-tune option-space digest.
- `tools/vmaf-tune/src/vmaftune/corpus.py` — Phase A JSONL emitter.
- `ai/src/vmaf_train/models/fr_regressor.py` — FRRegressor with
  `num_codecs` arg already plumbed (added by ADR-0235).
- Source: user task brief 2026-05-03 ("scaffold `fr_regressor_v2` —
  the codec-aware version of the FR regressor that consumes the JSONL
  corpus emitted by Phase A").
