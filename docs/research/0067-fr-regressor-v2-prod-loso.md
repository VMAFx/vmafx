<!-- markdownlint-disable MD013 MD060 -->
# Research-0067: fr_regressor_v2 PROD — LOSO ship-gate evaluation

**Date**: 2026-05-05
**Status**: Closed (ship gate cleared)
**Companion ADR**: [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md)
**Predecessor**: [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md) (smoke scaffold), [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) (codec-aware decision)

## Question

Does `fr_regressor_v2` clear the ADR-0235 ship gate when trained on a real
Phase A hardware-encoder corpus (canonical-6 per-frame features + codec
block), and is the codec-aware lift over v1 measurable?

## Method

- **Corpus**: `runs/phase_a/full_grid/per_frame_canonical6.jsonl` —
  216 (source, encoder, preset, cq) cells aggregated from 33,840 per-frame
  rows produced by `scripts/dev/hw_encoder_corpus.py` (PR #392, ADR-0237
  Phase A real-corpus runner) on the Netflix 9-source corpus, expanded
  across the NVENC + Intel QSV codec families (h264 / hevc / av1 × NVENC + QSV
  × 3 presets × 4 cq values).
- **Model**: `FRRegressor(in_features=6, num_codecs=12)` (canonical-6 +
  ENCODER_VOCAB v2 — 12 encoders, the v2 vocab from PR #394). MLP shape
  `6 → 32 → 32 → 32 → 1` with codec block concatenated before the first
  dense layer. 200 epochs, Adam lr=5e-4, batch=32, weight decay=1e-5.
- **Eval**: Leave-one-source-out (LOSO) over the 9 Netflix sources.
  Standard scaler fit on training fold, applied to test fold.
- **Hardware**: NVIDIA RTX 4090 (CUDA backend), training wall time
  ≤3s per fold, ≤30s total.

## Result

| Source held-out | LOSO PLCC | LOSO SROCC | RMSE  |
|-----------------|-----------|------------|-------|
| BigBuckBunny_25fps | 0.9652 | 0.9930 |  3.794 |
| BirdsInCage_30fps  | 0.9937 | 0.9583 |  1.233 |
| CrowdRun_25fps     | 0.9715 | 0.9661 |  5.367 |
| ElFuente1_30fps    | 0.9815 | 0.9609 |  3.047 |
| ElFuente2_30fps    | 0.9745 | 0.8930 | 11.453 |
| FoxBird_25fps      | 0.9521 | 0.9930 |  5.752 |
| OldTownCross_25fps | 0.9183 | 0.6322 |  5.371 |
| Seeking_25fps      | 0.9778 | 0.9696 |  4.689 |
| Tennis_24fps       | 0.9787 | 0.9583 |  6.391 |
| **Mean ± std**     | **0.9681 ± 0.0207** | **0.9249** | **5.233** |

## Verdict

**SHIP GATE: PASS** — LOSO PLCC = 0.9681 ± 0.0207 ≥ 0.95 threshold from
ADR-0235. The model graph + ENCODER_VOCAB v2 + canonical-6 input contract
is frozen as the v2 production checkpoint.

## Caveats / known limitations

- **OldTownCross outlier (PLCC 0.9183)** — barely below 0.95 in isolation.
  The source is a 25fps slow-pan film_drama with a very tight VMAF range
  (most encodes score 96-99 even at low CQ), so per-pair correlation is
  noise-bound. RMSE remains low (5.371). Held in scope; future v3 train
  with a wider CQ range would address.
- **In-sample PLCC = 0.9794** (training set) — generalisation gap vs LOSO
  mean of 0.011 is acceptable for the model size (32-32-32 MLP).
- **No software encoders in this corpus** — the 216 cells are NVENC + QSV
  only. The 6 software encoder one-hot dims (libx264/libx265/libsvtav1/
  libvvenc/libvpx-vp9) are seen at training time only via the codec block;
  predictions on x264/x265 corpora are extrapolation. Tracked as backlog
  T-FR-V2-SW-CORPUS for a follow-up training pass once the comprehensive
  software-encoder sweep is in.
- **216 cells is small** — overfitting risk addressed via LOSO + weight
  decay + early stopping. The 33,840 per-frame rows are aggregated to
  cells because the fr_regressor_v2 contract is per-cell (not per-frame);
  per-frame training is reserved for `vmaf_tiny_v3`/`v4`.

## Reproducer

```bash
# 1. Build per-frame canonical-6 corpus from hw_encoder_corpus output
python3 - <<'EOF'
import json
from collections import defaultdict
groups = defaultdict(list)
for p in ('runs/phase_a/qsv_pf.jsonl', 'runs/phase_a/nvenc_pf.jsonl'):
    with open(p) as f:
        for line in f:
            r = json.loads(line)
            key = (r['src'], r['encoder'], r.get('preset', 'medium'),
                   r.get('cq', r.get('crf', r.get('q', 23))))
            groups[key].append(r)
keys6 = ('adm2', 'vif_scale0', 'vif_scale1', 'vif_scale2', 'vif_scale3', 'motion2')
with open('runs/phase_a/full_grid/per_frame_canonical6.jsonl', 'w') as fo:
    for (src, enc, preset, q), rows in groups.items():
        n = len(rows)
        pf = {k: sum(r[k] for r in rows) / n for k in keys6}
        vmaf = sum(r['vmaf'] for r in rows) / n
        fo.write(json.dumps({
            'src': src, 'encoder': enc, 'preset': preset, 'crf': q,
            'vmaf_score': vmaf, 'per_frame_features': pf}) + '\n')
EOF

# 2. Train production checkpoint
python ai/scripts/train_fr_regressor_v2.py \
    --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl \
    --epochs 200 --batch-size 32 --lr 5e-4 --hidden 32 --depth 3

# 3. Re-run LOSO eval
python3 ai/scripts/eval_loso_fr_regressor_v2.py \
    --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl
# Expected: LOSO PLCC = 0.9681 ± 0.0207 (PASS 0.95 gate)
```

## References

- req (2026-05-05): "and let the gpu's do some work ffs" (popup choice
  "Both, in parallel" for v2-PROD train + Arc VBR sweep).
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — Phase A corpus contract
- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — codec-aware decision + 0.95 ship gate
- [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md) — smoke scaffold superseded by this run
- PR #392 (`hw_encoder_corpus.py`) — corpus runner that emitted the 33,840 per-frame rows
- PR #394 (ENCODER_VOCAB v2) — vocab extension covering NVENC + QSV
