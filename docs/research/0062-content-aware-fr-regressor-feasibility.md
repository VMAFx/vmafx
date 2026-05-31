<!-- markdownlint-disable MD013 -->
# Research-0062 — Content-aware fr_regressor_v2 feasibility

## Question

Can adding source content-class one-hot (animation / sports / film_drama
/ wildlife / ugc / unknown) to the `fr_regressor_v2` codec_block lift
PLCC on the Phase A real corpus (216 rows, 9 Netflix sources × 6 hw
codecs × 4 CQ values) over the codec-only baseline?

## Method

1. Train baseline: `fr_regressor_v2` with `ENCODER_VOCAB` v2 (12
   encoders + unknown) — see PR #394.
2. Extend `_row_to_features` to append a 6-dim content-class one-hot
   to `codec_block`. Tag the 9 Netflix sources by genre (manual):

   ```text
   BigBuckBunny → animation
   CrowdRun, Tennis → sports
   Seeking, ElFuente1, ElFuente2, OldTownCross → film_drama
   BirdsInCage, FoxBird → wildlife
   ```

3. Retrain with identical hyperparameters: 200 epochs, batch 32, lr 5e-4.

## Result

| config                     |   PLCC |  SROCC |  RMSE |
|----------------------------|-------:|-------:|------:|
| codec-only (vocab v2)      | 0.9599 | 0.9455 |  4.15 |
| codec + content_class      | 0.9609 | 0.9471 |  4.61 |

PLCC flat (Δ = +0.001, well below noise floor). RMSE **regressed**
by 0.46 VMAF units. The content_class one-hot is mostly noise on
this corpus.

## Why it doesn't lift

The 216-row corpus has ~9 rows per `(genre, codec, cq)` cell. With
6 content classes added as one-hot dims, the model has more
parameters to fit but the same training signal — overfit risk
without generalisation gain. The flat in-sample PLCC + worse RMSE
is the classic capacity-without-data symptom.

## What would lift PLCC

1. **Larger corpus** — full NVENC + QSV grid against 70 Netflix dis
   YUVs (not just 9 ref sources, but all reference / distorted pairs)
   would 7-10x the row count. Per-`(genre, codec, cq)` density
   moves from 9 rows to 60-90 rows.
2. **Per-frame features** instead of aggregate — currently
   `hw_encoder_corpus.py` produces per-frame data but the training
   loop averages it. Wiring per-frame canonical-6 + per-frame
   content-class through the loss would 150x the row count.
3. **Cross-corpus generalisation gate** — content_class signal might
   show up at LOSO (leave-one-source-out) eval rather than in-sample
   PLCC. The current `--metrics-out` reports in-sample only; a
   LOSO harness over the genre dimension would isolate the lift.
4. **Auto-extracted content features** instead of manual genre tags —
   spatial complexity (var/edge density), temporal complexity (motion
   magnitude — already in canonical-6), color distribution stats. A
   3-4 dim continuous feature is more efficient than a 6-dim one-hot.

## Decision

Park the content_class extension. Re-evaluate when:

- The corpus exceeds ~2000 rows (10x current), OR
- A LOSO eval surfaces a per-genre PLCC gap that content_class would close, OR
- Auto-extracted content features are available (Research-0063 candidate)

The append-only `CONTENT_CLASS_VOCAB` shape is documented here so
future runs can wire it without redoing the design.

## References

- PR #394 — `fr_regressor_v2` ENCODER_VOCAB v2 (codec extension that lifted PLCC 0.92 → 0.96)
- PR #392 — `scripts/dev/hw_encoder_corpus.py` (the data source)
- ADR-0237 — Phase A scope (vmaf-tune capability roadmap)
