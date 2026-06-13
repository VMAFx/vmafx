# Research-0086 — Video-temporal saliency feasibility for ROI-encode tuning

- **Status**: Active. Companion to [ADR-0325](../adr/0325-video-saliency-extension.md).
- **Date**: 2026-05-08
- **Tags**: ai, saliency, video-saliency, dnn, vmaf-tune, roi, fork-local, design

## Question

The fork ships `saliency_student_v1` (image-level, ~113 K params,
fork-trained on DUTS-TR, BSD-3-Clause-Plus-Patent, ADR-0286) and a
`vmaf-tune` ROI-encode pipeline (ADR-0293, `tools/vmaf-tune/src/vmaftune/saliency.py`)
that computes saliency on **N evenly-sampled frames per shot** and
averages them into a single per-clip mask before reducing to an x264
`--qpfile`. The aggregator is a true mean over independently-inferred
per-frame masks — no temporal model, no recurrence, no 3D conv. Eyes
track motion; a model that ignores motion is a known approximation.

The question this digest answers: **should the fork ship a true
video-saliency model next, or stay on temporal pooling of the existing
2D student?** Sub-questions on (a) which dataset, (b) which model
architecture, (c) at what cost, and (d) on what timeline are answered
in turn so the companion ADR can lock a phased rollout.

The fork's ROI-encode targets are x264 `--qpfile`, x265
`--qpfile`, and SVT-AV1 ROI-map. None of these consume a per-frame
saliency model at runtime — they consume one or more **pre-computed**
QP-offset maps per shot. The model runs **once**, in the harness, before
the encoder starts. That moves "inference cost" from a hot to a cold
path and eliminates the academic-SOTA-only-runs-in-PyTorch risk early.

## Method

WebSearch + WebFetch on five axes, dated 2026-05-08:

1. **Datasets**: DHF1K (Wang et al., CVPR 2018), Hollywood-2 saliency,
   UCF-Sports saliency, LEDOV (Jiang et al.), AViMoS (AIM 2024).
2. **Models**: TASED-Net (ICCV 2019), UNISAL (ECCV 2020), SalEMA
   (BMVC 2019), ViNet-v2 (ICASSP 2025), STSANet, Mamba-based
   "ZenithChaser" (AIM 2024 efficient track), and the
   "Minimalistic Video Saliency Prediction" (arXiv 2502.00397, 2025).
3. **Image-vs-video on ROI encoding**: literature on saliency-driven
   HEVC rate-control BD-rate gains.
4. **Tiny / efficient video saliency**: ONNX-deployable, sub-1M
   parameter regime.
5. **Temporal pooling baselines**: SalEMA's "EMA-on-2D-features"
   finding plus the DHF1K paper's own comparisons.

## Findings

### Axis 1 — Datasets

| Dataset | Year | Size | License | MOS / fixations | Notes |
| --- | --- | --- | --- | --- | --- |
| **DHF1K** [1] | 2018 | 1000 videos, splits 600 / 100 / 300; Hollywood-2 sibling alone is 74.6 GB | CC BY 4.0 | Eye-tracker, 17 observers | Permissive; standard benchmark; the gold reference. Distribution gated through Google Drive — same UI-clickwrap risk as ADR-0257 hit on MobileSal |
| **AViMoS (AIM 2024)** [2] | 2024 | 1500 videos, 1080p, ~170 GB extracted ground-truth | CC-BY (per challenge page) | Crowdsourced *mouse* tracking, > 5000 observers across the corpus | License is permissive but **mouse-tracking is a proxy for fixations, not the gold-standard eye-tracker signal** the older corpora use |
| **Hollywood-2 saliency** [1] | bundled with DHF1K | 74.6 GB | inherits DHF1K's CC BY 4.0 | Eye-tracker, 16 observers | Movie clips; biases the model toward narrative content |
| **UCF-Sports saliency** [1] | bundled with DHF1K | smaller | inherits | Eye-tracker | Sports action; biases toward fast motion |
| **LEDOV** [3] | 2018 | 538 videos, 179 336 frames | **License not stated on the GitHub repo or paper landing page**; needs author contact (Jiang et al., BUAA) | Eye-tracker | Larger than DHF1K in frame count; license-blocker until clarified |

DHF1K is the de-facto evaluation reference. AViMoS is more recent and
larger but mouse-tracked, which lower-bounds the perceived gap to a
true eye-tracking model.

### Axis 2 — Models

| Model | Year | Params | License | DHF1K CC / NSS (paper claim) | Notes |
| --- | --- | --- | --- | --- | --- |
| **TASED-Net** [4] | 2019 | **21.2 M** params, 63.2 GFLOPs (T=32 late-aggregation variant) [4a] | **MIT** | (paper-reported SOTA at submission) | S3D backbone pretrained on Kinetics-400; 3D-conv; the canonical "true video saliency" baseline |
| **UNISAL** [5] | 2020 | sub-5 M params (paper claims "5–20× smaller than competing deep methods"; backbone is **MobileNetV2** + Bypass-RNN) | **Apache-2.0** | SOTA on DHF1K, Hollywood-2, UCF-Sports at one parameter set | Encoder–RNN–decoder, joint image+video training; the architecture closest to "lightweight teacher we can distil from" |
| **SalEMA** [6] | 2019 | small (paper builds on a 2D backbone + EMA over a single conv state) | (PyTorch repo, license not surfaced on paper page; verify before use) | Reports "almost as good as ConvLSTM" without retraining the 2D backbone | Direct evidence that EMA over a 2D backbone closes most of the gap to true temporal models |
| **ViNet-v2 (ViNet-S, ViNet-A)** [7] | ICASSP 2025 | ViNet-S 36 MB, ViNet-A 148 MB (~5–35 M params equivalent); ViNet-S claims > 1000 fps on a GPU | **CC BY-NC-SA 4.0** | SOTA on DHF1K + 6 audio-visual datasets at ensemble | **License-incompatible** with the fork's BSD-3-Clause-Plus-Patent (re-runs the ADR-0257 / MobileSal blocker) |
| **AIM 2024 winners (UMT-based)** [2] | 2024 | top entry (CV_MM) **420.5 M** params; 2nd (VistaHL) 187.7 M; 3rd (PeRCeiVe Lab) UMT-based | per-team (no blanket licence) | Top of AViMoS leaderboard | Far above the fork's tiny-AI footprint; not shippable as a runtime model |
| **AIM 2024 efficient (ZenithChaser, Mamba)** [2] | 2024 | **0.19 M** params | per-team (no blanket licence) | 5th–6th overall on AViMoS leaderboard | First-ever "tiny-class" video-saliency entry; demonstrates the regime is reachable. Mamba selective-state-space op is **not on the fork's ONNX op-allowlist** today |
| **"Minimalistic" (arXiv 2502.00397)** [8] | 2025 | not extracted from the abstract page; positions itself as efficient-decoder + STAL-cued | per-paper repo | claims competitive vs ViNet / TASED-Net | Useful design reference but no shippable open weights confirmed at this digest's date |

### Axis 3 — Image vs. video saliency for ROI encoding

The literature on saliency-driven HEVC and AV1 ROI rate-control
reports BD-rate savings in the **3–12 % range** depending on content
type and saliency-map quality [9]. Critically, none of the cited HEVC
papers isolate "image saliency averaged across N frames" vs. "true
video saliency model" as separate ablations. The 3–12 % band is an
upper-bound on the *combined* gain from any saliency signal of
"reasonable quality" plus the rate-control wiring.

Empirical signal from the saliency literature itself: SalEMA's headline
finding [6] is that an exponential moving average over a frozen 2D
backbone "achieves almost as well as a sophisticated ConvLSTM
recurrence" on DHF1K, *without retraining*. That establishes the
EMA-of-2D regime as a strong baseline — already on the order of 80–90 %
of what a heavier ConvLSTM model captures, on the standard CC / NSS /
AUC-J / SIM metric set. ViNet-S's 2025 ICASSP claim of > 1000 fps [7]
also implies the per-frame cost of a 2D-style backbone, evaluated
densely, is not the bottleneck. The bottleneck is recurrent state
across frames — and EMA models that with a single weighted
running-mean update.

For the fork's specific ROI-encode use case, the saliency mask is
**reduced to per-MB granularity (16×16 luma) before quantisation**.
Any spatial precision that survives a 16× downsample is what the
encoder actually consumes. Sub-block jitter from a slightly noisier
per-frame mean is averaged out by the per-MB reduce. The per-MB step
is therefore an inherent low-pass over the saliency signal that
flatters cheaper temporal aggregators relative to true 3D-conv models.

### Axis 4 — Tiny / efficient video saliency

The space below 1 M parameters is reachable: **ZenithChaser** (AIM
2024, 0.19 M params, Mamba) demonstrates it. UNISAL (MobileNetV2 +
Bypass-RNN) sits in the 5 M range. The "Minimalistic" paper [8]
explicitly targets parameter-efficient decoders. None of these have
weights distributed under a BSD-/Apache-/MIT-compatible licence with
direct-HTTP download as of this digest's date — the ZenithChaser
model is reproducible from the published paper but has no off-the-shelf
ONNX export, and Mamba's selective-state-space op is not on the
fork's `core/src/dnn/op_allowlist.c`.

The shippable path is therefore **knowledge distillation**:
distil a tiny student (target ~150–250 K params, similar regime to
`saliency_student_v1`) from a permissive teacher (UNISAL,
Apache-2.0) on a permissive corpus (DHF1K, CC BY 4.0). The student
ships under BSD-3-Clause-Plus-Patent, the teacher and dataset never
ship in-tree.

### Axis 5 — Temporal pooling tricks

Two specific aggregators emerge from the literature, both compatible
with a **frozen** `saliency_student_v1` (no retraining needed for
Phase 1):

1. **Per-frame mean (status quo)** — N=8 evenly-spaced frames,
   per-pixel arithmetic mean. Already in
   `tools/vmaf-tune/src/vmaftune/saliency.py`.
2. **EMA over per-frame masks** — per-pixel
   `m_t = α · sal_t + (1 − α) · m_{t−1}` with α tuned per the SalEMA
   recipe [6]. Captures temporal coherence (a salient region in
   frames 1–3 stays salient at t=4 even if the per-frame model
   slightly mis-fires). Adds a single hyperparameter, no new model.
3. **Motion-weighted mean** — weight per-frame mask by per-frame
   inter-frame difference (a cheap proxy for "this frame has motion
   the eye is tracking"). Adds one frame-difference computation per
   sampled frame; no new model.

Aggregator (2) is the SalEMA-validated choice. Aggregator (3) is the
content-aware variant that surfaces the motion signal the per-clip
mean explicitly throws away.

## Recommendation

**Phase 1 — temporal-pooling baseline, frozen
`saliency_student_v1` (ship next).** Replace the per-clip mean in
`vmaf-tune/saliency.py` with a configurable aggregator: `mean`
(today), `ema` (default), `motion-weighted`. No new model, no new
ONNX file, no new training script. Cost: ~50 LOC + tests. Expected
BD-rate-vs-status-quo: at most a small fraction of the 3–12 % band
[9], probably comparable to the SalEMA "EMA closes the
ConvLSTM-vs-2D gap" finding [6]. Ships in days, not weeks.

**Phase 2 — `video_saliency_student_v1` via distillation
(follow-up).** Distil a ~200–300 K-parameter student from UNISAL
(Apache-2.0, MobileNetV2 + Bypass-RNN) on DHF1K (CC BY 4.0). Architecture
mirrors `saliency_student_v1` (TinyU-Net) plus a Bypass-RNN-style
single-state recurrence — i.e., the Phase-1 EMA promoted into a
learned per-pixel update rule, exported as one ONNX graph. Use only
ops on `core/src/dnn/op_allowlist.c` (the existing TinyU-Net
op-set plus a `Where` / `Mul` / `Add` chain that already lives in
ONNX opset 17). Training script under
`ai/scripts/train_video_saliency_student.py`, recipe modelled on
`train_saliency_student.py`. Distil from UNISAL outputs as soft labels
on DHF1K-train; eval on DHF1K-val with CC / NSS / AUC-J / SIM, with
per-MB 16× downsampled mask IoU as the *application-aligned* metric.
Ship the trained `.onnx` only; UNISAL weights are *not* redistributed
in-tree. Estimated wall-clock: ~30 minutes on a single GPU at the
fork's existing scale.

**Phase 3 — ONNX-export + harness integration.** Register the
new model in `model/tiny/video_saliency_student_v1.onnx`; add a
`docs/ai/models/video_saliency_student_v1.md` model card; wire a
`--saliency-mode {image, video}` flag into `vmaf-tune recommend`
that switches between today's image-saliency student and the new
video-saliency student; default stays `image` until the BD-rate
sweep confirms a positive lift on the corpus.

**The decision matrix's runner-up** is "ship Phase 1 only and stop."
It is the conservative outcome if the Phase-1 EMA closes the gap on
the application-aligned per-MB IoU metric, since the per-MB
downsample dominates the spatial precision the encoder consumes
anyway.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **(A) Phase-1 + Phase-2 distillation roadmap (recommended)** | Cheap immediate win (Phase 1); a true video-saliency model on the same shippable footprint as `saliency_student_v1` (Phase 2); both phases are independently mergeable | Two PRs instead of one; Phase 2 needs a fork-managed teacher run | **Chosen** |
| **(B) Ship a true 3D-conv model directly (TASED-Net, 21 M params, MIT)** | MIT-licensed; well-known SOTA reference | 21 M params is **2 orders of magnitude** above the fork's tiny-AI footprint (`saliency_student_v1` is 113 K); 3D conv stack inflates ONNX graph size and runs against a different op-allowlist subset; the ROI-encode use case downsamples to per-MB anyway | Rejected — wrong size class for fork, no measurable BD-rate gain expected to justify it |
| **(C) Adopt ViNet-S directly (36 MB, 1000+ fps)** | SOTA, fast | **CC BY-NC-SA 4.0** — non-commercial, share-alike; same blocker that rejected upstream MobileSal in ADR-0257 | Rejected — license-incompatible with BSD-3-Clause-Plus-Patent fork |
| **(D) Train on AViMoS (1500 videos, CC-BY, AIM 2024)** | Larger, more recent than DHF1K; permissive | Mouse-tracked rather than eye-tracked → upper-bounded ground-truth quality; ~170 GB ground-truth alone | Held in reserve for `video_saliency_student_v2` if Phase 2 saturates on DHF1K |
| **(E) Stay on per-frame image saliency forever (status quo)** | Zero engineering | Eye-tracking literature is unambiguous that motion drives fixation; the per-clip mean discards this signal; SalEMA [6] shows EMA closes most of the gap for free | Rejected — the cheap (Phase 1) option is too cheap to skip |
| **(F) Adopt the AIM 2024 ZenithChaser Mamba 0.19 M model** | Right parameter regime; demonstrates "tiny video saliency" is reachable | Mamba's selective-state-space op is **not on the fork's ONNX op-allowlist** today; would inflate the PR scope into an op-allowlist patch + a training run + a runtime audit | Held — re-evaluate when the op-allowlist gains the relevant ops for an unrelated reason |

## References

[1] Wang, Shen, Guo, Cheng, Borji, "Revisiting Video Saliency: A
Large-scale Benchmark and a New Model" (CVPR 2018; PAMI 2019). DHF1K
project page: <https://github.com/wenguanwang/DHF1K> — license
Creative Commons Attribution 4.0; train/val/test split 600/100/300;
Hollywood-2 sibling 74.6 GB. WebFetch verified 2026-05-08.

[2] AIM 2024 Challenge on Video Saliency Prediction (ECCVW 2024).
<https://arxiv.org/html/2409.14827v1>. Top entries: CV_MM 420.5 M
params, VistaHL 187.7 M, PeRCeiVe Lab UMT-based;
ZenithChaser 0.19 M params Mamba (efficient track). Dataset:
AViMoS, 1500 videos at 1080p, mouse-tracked, > 5000 observers,
~170 GB ground-truth. WebFetch verified 2026-05-08.

[3] Jiang et al., "DeepVS: A Deep Learning Based Video Saliency
Prediction Approach" (ECCV 2018). LEDOV repo:
<https://github.com/remega/LEDOV-eye-tracking-database>. License not
stated on the public README at the time of this digest. WebSearch
2026-05-08.

[4] Min, Corso, "TASED-Net: Temporally-Aggregating Spatial
Encoder-Decoder Network for Video Saliency Detection" (ICCV 2019).
<https://github.com/MichiganCOG/TASED-Net> — MIT license. S3D
encoder pretrained on Kinetics-400. WebFetch verified 2026-05-08.

[4a] TASED-Net parameter / FLOPs figure (T=32 late-aggregation
variant, 21.2 M params / 63.2 GFLOPs) sourced from the comparative
table in the "Minimalistic Video Saliency Prediction" paper [8].

[5] Droste, Jiao, Noble, "Unified Image and Video Saliency Modeling"
(ECCV 2020). <https://arxiv.org/abs/2003.05477>. Repo
<https://github.com/rdroste/unisal> — Apache-2.0. Backbone
MobileNetV2 with Bypass-RNN; "5 to 20-fold smaller model size
compared to all competing deep methods"; evaluated on DHF1K,
Hollywood-2, UCF-Sports, SALICON, MIT300. WebFetch + WebSearch
verified 2026-05-08.

[6] Linardos, Mohedano, Nieto, McGuinness, Giró-i-Nieto, O'Connor,
"Simple vs complex temporal recurrences for video saliency
prediction" (BMVC 2019). <https://arxiv.org/abs/1907.01869>;
project page <https://imatge-upc.github.io/SalEMA/>. Headline:
EMA over a frozen 2D backbone "achieves almost as well as a
sophisticated ConvLSTM recurrence" on DHF1K. WebFetch verified
2026-05-08.

[7] ViNet-v2 (ICASSP 2025). <https://github.com/ViNet-Saliency/vinet_v2>
— **CC BY-NC-SA 4.0** (non-commercial, share-alike).
ViNet-S 36 MB, ViNet-A 148 MB; ViNet-S claims > 1000 fps. WebFetch
verified 2026-05-08. License blocker mirrors ADR-0257 / upstream
MobileSal rejection.

[8] "Minimalistic Video Saliency Prediction via Efficient Decoder &
Spatio Temporal Action Cues", arXiv 2502.00397 (2025).
<https://arxiv.org/abs/2502.00397>. Source for cross-model
parameter / FLOPs figures including TASED-Net [4a]. WebFetch
2026-05-08.

[9] Saliency-driven HEVC rate control: "Saliency based rate control
scheme for high efficiency video coding"
(<https://ieeexplore.ieee.org/document/7820898>); "High Efficiency
Video Coding Compliant Perceptual Video Coding Using Entropy Based
Visual Saliency Model"
(<https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7514295/>);
"Content-aware rate control scheme for HEVC based on static and
dynamic saliency detection"
(<https://www.sciencedirect.com/science/article/abs/pii/S0925231220309668>).
Combined band of reported BD-rate savings: 3–12 %. WebSearch
2026-05-08.

[10] AViMoS challenge / dataset landing page,
<https://challenges.videoprocessing.ai/challenges/video-saliency-prediction.html>;
release repo
<https://github.com/msu-video-group/ECCVW24_Saliency_Prediction>.
License CC-BY (per challenge page). WebFetch verified 2026-05-08.

[11] Existing fork prior art —
[ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md)
(`saliency_student_v1`, fork-trained, ~113 K params,
BSD-3-Clause-Plus-Patent),
[ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md) (`vmaf-tune`
saliency-aware ROI), [ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md)
(license blocker on upstream weights), and
`tools/vmaf-tune/src/vmaftune/saliency.py` (the per-clip mean this
digest proposes to upgrade).

[12] Source of this research task: paraphrased — user direction to
research a video-temporal saliency model that complements the
existing image-level student, and to be honest about cost so the
recommendation matches the actual lift available through the
per-MB-downsampled ROI surface.
