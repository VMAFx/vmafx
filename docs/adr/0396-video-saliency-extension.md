# ADR-0396: Video-temporal saliency extension to `saliency_student_v1`

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, saliency, video-saliency, vmaf-tune, roi, fork-local, design

## Context

The fork ships an image-level saliency student
`saliency_student_v1` (~113 K parameters, fork-trained on DUTS-TR,
BSD-3-Clause-Plus-Patent — [ADR-0286](0286-saliency-student-fork-trained-on-duts.md))
and a `vmaf-tune` ROI-encode pipeline
([ADR-0293](0293-vmaf-tune-saliency-aware.md), implemented at
`tools/vmaf-tune/src/vmaftune/saliency.py`) that runs the student on
**N=8 evenly-spaced frames per shot, averages the per-frame masks**,
reduces to per-MB (16×16) granularity, clamps to ±12 QP offsets, and
writes an x264 `--qpfile`. The per-clip arithmetic mean is a known
approximation: human gaze tracks motion across frames, and a per-frame
model that is averaged afterwards discards exactly the temporal
coherence the encoder would benefit from.

[Research-0086](../research/0086-video-saliency-feasibility-2026-05-08.md)
surveys the video-saliency landscape (datasets DHF1K / AViMoS /
Hollywood-2 / UCF-Sports / LEDOV; models TASED-Net / UNISAL / SalEMA /
ViNet-v2 / AIM-2024 winners) and lands on three findings that drive
this ADR:

1. **EMA over a frozen 2D backbone closes most of the gap to a
   sophisticated temporal model on DHF1K** without retraining
   (SalEMA, BMVC 2019). The fork can capture this signal *immediately*
   by replacing the per-clip mean with an EMA aggregator inside
   `saliency.py` — zero new model, zero new ONNX, ~50 LOC.
2. **A true video-saliency model in the fork's tiny-AI footprint is
   reachable via knowledge distillation** from UNISAL (Apache-2.0,
   MobileNetV2 + Bypass-RNN, "5–20× smaller than competing deep
   methods", evaluated on DHF1K / Hollywood-2 / UCF-Sports). The
   teacher and dataset are both license-compatible; the student
   ships under BSD-3-Clause-Plus-Patent on the same training pattern
   as `saliency_student_v1`.
3. **The ROI-encode surface downsamples to per-MB anyway**: any
   spatial precision finer than 16 luma samples is averaged out
   before the encoder consumes it. The application-aligned metric is
   *per-MB IoU against the ground-truth saliency mask after the same
   16× reduce*, not raw CC / NSS at native resolution. That metric
   flatters cheaper temporal aggregators and lower-bounds the lift a
   true video model can deliver inside this pipeline.

These three findings split naturally into three phases. Phase 1 is
the immediate win that costs almost nothing. Phase 2 is the real
video-saliency student. Phase 3 wires Phase 2 into the harness
behind a feature flag.

## Decision

We will extend the fork's saliency stack with a video-temporal
surface in three independently-mergeable phases, each gated on the
prior phase's measured lift:

Status update 2026-05-15: Phase 1 is implemented in
`vmaftune.saliency.compute_saliency_map()` and exposed through
`vmaf-tune recommend-saliency --saliency-aggregator`. The default
remains `mean` for compatibility; `ema`, `max`, and
`motion-weighted` are opt-in baselines.

### Phase 1 — Temporal-pooling baseline (`saliency.py` aggregator)

Add a configurable temporal aggregator to
`tools/vmaf-tune/src/vmaftune/saliency.py`'s `compute_saliency_map`
function:

| Mode | Formula |
| --- | --- |
| `mean` (today's behaviour) | `m = mean(sal_t for t in sampled_frames)` |
| `ema` | `m_t = α · sal_t + (1 − α) · m_{t−1}`, with α exposed by `--saliency-ema-alpha` |
| `max` | `m = max(sal_t for t in sampled_frames)` |
| `motion-weighted` | `m = Σ w_t · sal_t / Σ w_t`, with `w_t = mean(abs(Y_t − Y_{t−1}))` (per-frame inter-frame difference as a motion proxy) |

Selectable via `SaliencyConfig.temporal_aggregator` and a
`vmaf-tune recommend-saliency --saliency-aggregator` CLI flag. The
default remains `mean` (status-quo-preserving); any future default
flip is a follow-up PR after a BD-rate sweep on the existing corpus.
No new model, no new ONNX, no new training script.

### Phase 2 — `video_saliency_student_v1` via UNISAL distillation

Train a new tiny student `video_saliency_student_v1` (target
**~200–300 K parameters**, ONNX opset 17,
BSD-3-Clause-Plus-Patent) by distilling from UNISAL on DHF1K:

- **Teacher**: UNISAL (Apache-2.0,
  <https://github.com/rdroste/unisal>). Run once, locally, to
  produce per-frame saliency maps on DHF1K-train. Teacher weights
  are *not* redistributed in-tree; only the trained student is.
- **Dataset**: DHF1K (CC BY 4.0). Splits 600 train / 100 val / 300
  held-out test. Dataset is *not* committed in-tree; the training
  script's docstring records the download URL — same pattern as
  `train_saliency_student.py` does for DUTS-TR.
- **Student architecture**: TinyU-Net (mirrors
  `saliency_student_v1`'s 3 down + 3 up encoder–decoder) plus a
  **Bypass-RNN-style single-state recurrence on the bottleneck
  feature map**. The recurrence is implemented as a learned
  per-channel EMA gate (one `Mul` + one `Add` per timestep, with the
  EMA coefficient itself a learned per-channel parameter). All ops
  must be on `core/src/dnn/op_allowlist.c` at the time of the PR
  — no new allowlist entries in the same PR.
- **Loss**: per-frame BCE + Dice on the saliency mask, plus a
  KL-divergence soft-label loss against the teacher's saliency
  output. Standard distillation recipe.
- **Eval**: standard CC / NSS / AUC-J / SIM on DHF1K-val *and*
  per-MB-IoU after 16× reduce as the application-aligned metric.
- **Training script**:
  `ai/scripts/train_video_saliency_student.py`, modelled on
  `ai/scripts/train_saliency_student.py`. Estimated ~30 minutes wall
  clock on a single GPU at the fork's scale; deterministic given
  `--seed` and pinned PyTorch / CUDA versions.

The video-saliency student exposes the **same I/O contract** as
`saliency_student_v1` plus one optional input: the bottleneck-state
tensor from the previous frame. When called single-frame the
recurrence input defaults to zero, so the model is a strict drop-in
for any consumer that ignores temporal state.

### Phase 3 — ONNX export + `vmaf-tune` integration

- Register `model/tiny/video_saliency_student_v1.onnx` via
  `/add-model` (ADR-0286 pattern).
- Ship `docs/ai/models/video_saliency_student_v1.md` (model card —
  ADR-0042's 5-point bar).
- Add `--saliency-mode {image, video}` to
  `vmaf-tune recommend`. `image` (today) routes through
  `saliency_student_v1` + Phase-1 EMA aggregator; `video` routes
  through `video_saliency_student_v1`'s native temporal recurrence
  with a per-frame fold over the bottleneck state.
- Default stays `image` until a corpus-level BD-rate sweep on the
  existing tuning corpus confirms a positive lift for `video`. The
  flip is its own follow-up ADR.
- Bit-for-bit numerical contract with `vmaf-roi` C sidecar (ADR-0247)
  is preserved by routing only the *saliency mask* through the new
  model and leaving the saliency→QP-offset map identical
  (`(2·sal − 1) · foreground_offset`, clamp to ±12).

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **(A) Phased rollout, EMA → distilled student → integration (this ADR)** | Cheap immediate win (Phase 1, days); a true video-saliency model on the same shippable footprint as `saliency_student_v1` (Phase 2); both phases are independently mergeable; default-flip is a separate ADR with measured BD-rate justification | Two follow-up PRs instead of one; Phase 2 needs a fork-managed teacher run | **Chosen** — see Research-0086 §Recommendation |
| **(B) Adopt TASED-Net directly (21.2 M params, MIT license)** | MIT-licensed; canonical 3D-conv reference; published metrics on DHF1K / Hollywood-2 / UCF-Sports | 21.2 M parameters is **two orders of magnitude above** `saliency_student_v1`'s 113 K — wrong size class for the fork's tiny-AI footprint; 3D-conv stack inflates ONNX graph size; the per-MB downsample dominates the spatial precision the encoder consumes anyway | Rejected — measurable BD-rate gain over Phase 1 is unlikely to justify the size jump |
| **(C) Adopt ViNet-v2 / ViNet-S (36 MB, > 1000 fps, ICASSP 2025)** | SOTA on DHF1K + 6 audio-visual datasets; very fast | **CC BY-NC-SA 4.0** — non-commercial, share-alike. Same blocker that rejected upstream MobileSal in [ADR-0257](0257-mobilesal-real-weights-deferred.md) | Rejected — license-incompatible with BSD-3-Clause-Plus-Patent |
| **(D) Train on AViMoS (1500 videos, AIM 2024 challenge data, CC-BY)** | Larger, more recent than DHF1K; permissive | Mouse-tracking (not eye-tracking) → upper-bounded ground-truth quality vs. DHF1K; ~170 GB ground-truth alone | Held in reserve — `video_saliency_student_v2` if Phase 2 saturates on DHF1K |
| **(E) Stay on per-frame image saliency forever** | Zero engineering | Eye-tracking literature is unambiguous that motion drives fixation; SalEMA shows EMA closes most of the gap "for free" | Rejected — Phase 1 is too cheap to skip |
| **(F) Adopt the AIM 2024 ZenithChaser Mamba 0.19 M model** | Right parameter regime for the fork; demonstrates "tiny video saliency" is reachable | Mamba's selective-state-space op is **not on the fork's ONNX op-allowlist** today; would inflate the PR scope into op-allowlist + training-run + runtime audit | Held — re-evaluate when the op-allowlist gains the relevant ops for an unrelated reason |
| **(G) Skip Phase 1 and go straight to Phase 2** | One PR instead of two; no aggregator / model split-personality during the transition | Loses the "almost-free" SalEMA win during the Phase-2 development time; couples the design decision (true video model is shippable) to the implementation milestone (model trains and exports cleanly); blocks the immediate measurement of "how much of the lift is just temporal smoothing?" | Rejected — Phase 1 is a measurement, not just a milestone |

## Consequences

### Positive

- The fork captures the SalEMA-validated temporal-coherence gain on
  the existing `saliency_student_v1` immediately, with no new model
  surface to maintain (Phase 1).
- Establishes a **distillation pattern** for fork-shippable
  tiny-AI models from permissive teachers — re-usable for future
  models (saliency_student_v3, content-class predictor, …).
- The video-saliency student is **a strict drop-in superset** of
  the image student: any consumer that ignores temporal state still
  gets a valid per-frame saliency mask. No flag-day migration.
- The application-aligned per-MB-IoU metric is a fork-owned
  evaluation surface: future improvements measure against what the
  encoder actually sees, not against a perceptual-saliency
  benchmark whose finer-than-16-luma precision the pipeline throws
  away.

### Negative

- Phase 2 introduces a new training corpus dependency (DHF1K, ~80 GB
  download) and a new teacher dependency (UNISAL, ~5 M params, run
  once locally). Mitigated by recording the download URLs in the
  training-script docstring, mirroring the DUTS-TR pattern from
  `train_saliency_student.py`, and *not* redistributing either
  in-tree.
- Phase 2 ships a second saliency model in `model/tiny/`, doubling
  the saliency-related ONNX surface area on disk. ~250 K params at
  fp32 is ~1 MB — bounded. The image student is kept as the default
  until BD-rate evidence justifies the flip.
- The temporal recurrence in Phase 2 makes the model **stateful**
  across frames at inference time. Single-frame consumers (the
  image-saliency `feature_mobilesal.c` C-side path) must initialise
  the recurrence state to zeros — handled by the I/O contract's
  default value, but a documentation hazard for new consumers.

### Neutral / follow-ups

- Default `--saliency-mode image` → `video` flip is its own
  follow-up ADR with a corpus-level BD-rate sweep as evidence.
- AViMoS-trained `video_saliency_student_v2` is filed as a backlog
  row, gated on the Phase-2 model saturating on DHF1K-val.
- Per-MB-IoU evaluation harness is reusable for any future saliency
  model the fork ships; lives under `ai/scripts/eval_saliency_per_mb.py`
  (Phase 2 deliverable, lands with the training script).
- C-side `feature_mobilesal.c` is **not** changed by this ADR — the
  video model is consumed only by the Python harness in Phase 3.
  Wiring the recurrent state into the C-side extractor is a separate
  ADR if and when a libvmaf consumer asks for it.

## References

- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) —
  `saliency_student_v1` (image-level, DUTS-TR-trained,
  BSD-3-Clause-Plus-Patent). The fork-shippable-tiny-AI-from-permissive-corpus
  pattern this ADR generalises to video.
- [ADR-0293](0293-vmaf-tune-saliency-aware.md) — `vmaf-tune`
  saliency-aware ROI-encode pipeline (Phase 1 modifies this surface;
  Phase 3 adds a flag here).
- [ADR-0247](0247-vmaf-roi-tool.md) — `vmaf-roi` C sidecar
  (saliency→QP-offset numeric contract preserved by Phase 3).
- [ADR-0218](0218-mobilesal-saliency-extractor.md) —
  `feature_mobilesal.c` C-side extractor (unchanged by this ADR).
- [ADR-0257](0257-mobilesal-real-weights-deferred.md) — license
  blocker on upstream MobileSal weights; ViNet-v2 is rejected on the
  same grounds (CC BY-NC-SA 4.0).
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule. Phase 3 ships
  `docs/ai/models/video_saliency_student_v1.md`.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables checklist (this PR is research-only).
- [Research-0086](../research/0086-video-saliency-feasibility-2026-05-08.md)
  — the companion digest with dataset / model / cost survey.
- Source: paraphrased — task brief directive to "research and design
  a video-temporal saliency model to complement the fork's existing
  `saliency_student_v1`", ship a phased rollout, and stay honest
  about the cost-vs-lift trade-off given the per-MB downsample the
  ROI surface already imposes.
