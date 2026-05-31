# Research-0086 — Tiny-AI SOTA deep dive: is the lusoris approach state of the art?

- **Status**: Active — strategic input. Every quantitative claim is web-sourced
  with date-stamped citations; no fabricated PLCC/SROCC numbers. Where a number
  could not be verified the entry is tagged `[UNVERIFIED]`.
- **Author**: research-subagent (Opus 4.7, 1 M context)
- **Date**: 2026-05-08
- **Tags**: tiny-ai, fr_regressor_v2_ensemble, NR-VQA, vmaf-tune, predict-then-verify
- **Workstream parent**: tiny-AI strategy ("the real cheese") — not yet ADR-bound,
  this digest exists to inform the next ADR round
- **Pairs with**: ADR-0303 (production-flip gate), ADR-0291 (probabilistic head),
  ADR-0309 (ensemble retrain), ADR-0310 (BVI-DVC ingestion), ADR-0042 (tiny-AI
  per-PR doc bar)

---

## TL;DR

- **Where we are clearly SOTA-adjacent (matched, not exceeded):** our
  `fr_regressor_v2_ensemble` (codec-aware, ensemble-of-5, ~5–20 K params)
  occupies the *same niche* as **Synamedia/Quortex pVMAF** (PLCC 0.985 /
  SROCC 0.988 sequence-level, shallow MLP, encoder-loop features) and
  **MainConcept VMAF-E** (±2 VMAF, 10× faster, neural net, in-encode-loop) —
  three known industry analogues, all 2024–2025, none open-source under
  permissive licences. We are competitive; we are *not* uniquely SOTA on raw
  accuracy, and we have no published external benchmark that lets us claim a
  ranked win. Predicted-VMAF surrogates are now a **crowded niche**.
- **Where we are clearly BEHIND:** **NR-VQA** (no-reference). DOVER-Mobile
  (9.86 M params, PLCC 0.853 KoNViD / 0.867 LSVQ_test, 1.4 s CPU per video)
  and **Q-Align** (LMM-based, ICML 2024) are the deployable open-weight
  baselines on UGC. We have `nr_metric_v1` (~19 K params, KoNViD-1k only) and
  no published external benchmark — at least an order of magnitude smaller
  but also untested on the canonical leaderboards. KonViD-150k is the right
  corpus to start; **LSVQ (39 K videos, 5.5 M ratings)** is the *bigger* one
  the field actually trains on.
- **Where we are GENUINELY NOVEL (no clear prior art on the exact pattern):**
  (a) the **predict-then-verify loop with GOSPEL/RECALIBRATE/FALL_BACK
  verdicts** as a deployment pattern — the closest analogue is two-pass
  per-shot encoding (Wu et al. 2022) but no one publishes the explicit
  three-state verdict semantic; (b) the **ADR-0303 production-flip gate**
  (mean-PLCC + max-min spread on LOSO ensemble as a *deployment* gate) — no
  external paper or vendor blog uses that two-criterion shape; (c) the
  combination **Sigstore-keyless-signed ONNX + ONNX op allowlist + per-model
  PTQ accuracy budget** as a *standard* — sigstore/model-transparency exists
  but no one ships a permissively-licensed VQA stack with this whole stack
  wired into CI; (d) **codec-aware FR with a 16-slot ENCODER_VOCAB v3
  one-hot** — the field generally trains *one* model per codec rather than
  conditioning, which is empirically a wash but architecturally more
  reusable.
- **Biggest single threat to our differentiation:** **MainConcept vScore /
  VMAF-E** (Sept 2025) and **Synamedia x264-pVMAF** (Nov 2024 OSS, GPL-2.0)
  ship to the same audience. Once a GPL-2.0 reference exists, our BSD/permissive
  stack is the only durable moat — pVMAF can't be linked into proprietary
  pipelines that ours can.
- **Most surprising finding (positive):** the **field has NOT solved
  community-uploaded VQ data**. CHUG (2025, 856 videos, 211 K AMT ratings)
  is the *only* open-call UGC-HDR dataset of its kind and it relied on AMT
  for ratings, not a federated/online learner. The "community contributes
  encode results, model improves" pattern lusoris is sketching has **no
  published precedent** I could find — that direction is genuinely open.
- **Most surprising finding (negative):** SVT-AV1-PSY (the perceptual
  fork) was **archived 2025-04-20**; the maintainer is upstreaming work
  into mainline SVT-AV1 (juliobbv-p/svt-av1-hdr is the recommended
  continuation). Our saliency-aware ROI x264 path is *more advanced than
  any open-weight AV1 saliency-ROI path that currently ships*, because the
  AV1 ecosystem just lost its psy-tuned fork.

---

## Topic-by-topic

### 1. Tiny full-reference VMAF prediction

**What the field is doing (2024–2026):**

- **Synamedia / Quortex pVMAF** — shallow MLP (1–2 hidden layers, neural
  features include QPs + pre-analysis stats + PSNR), claimed PLCC 0.985 /
  SROCC 0.988 *sequence-level* against full VMAF, MAE 1.34, RMSE 2.20;
  CPU overhead ~0.06 % during FHD medium-preset encode. **x264-pVMAF**
  released as **GPL-2.0 OSS** at <https://github.com/quortex/x264-pVMAF>
  (Nov 2024); 35× faster than VMAF, frame-level SROCC 0.991, FHD-only,
  inference C-code, **no training code, no weights detail published**.
- **MainConcept VMAF-E (vScore suite)** — neural net, ±2 VMAF accuracy,
  up to 10× faster than VMAF, integrated into Codec SDK 16.0
  (Sept 2025), in-coding-order so true real-time. Closed source.
- **Huawei PyTorch VMAF re-implementation** (Sept 2023, arXiv
  2310.15578) — full-pipeline reproduction (VIF, ADM, motion + SVR-RBF)
  with discrepancy ≲ 10⁻² VMAF units, purpose-built for *gradient-based
  optimisation of preprocessing filters*. Not a tiny student. Code
  promised, **not yet released** as of latest version.
- **Ren et al. — per-title CRF DNN** (Expert Systems with Applications
  2023, doi 10.1016/j.eswa.2023.120469) — DNN regressor that predicts
  CRF for a target VMAF from per-segment features; targets VP9; reports
  ~1 VMAF MAE on the test split.
- **Constant Target Quality (MainConcept blog 2023)** — same family,
  proprietary.
- **Direct VMAF-distillation papers**: explicit search for `"VMAF
  distillation"` returned **no published paper** beyond preprocessing-
  network work (Antsiferova et al., ResearchGate 376439157, 2023, who
  use a *trained VMAF approximation* as a proxy *teacher* for a U-Net
  preprocessor — the inverse of what we do). I.e. *no public paper
  distills VMAF into a tiny student as a metric in itself*. The closest
  is pVMAF's blog post, which is a marketing surface, not a paper.

**Where we stand:**

Our `fr_regressor_v2_ensemble` (5 seeds, 6 canonical libvmaf features +
18-D codec block, mean LOSO PLCC ≥ 0.95 per ADR-0303 gate, ~5–20 K params
per seed) is **structurally identical to pVMAF**: shallow MLP, encoder-
adjacent features, full-VMAF teacher. The numerical comparison is
roughly:

| Predictor | Params | Inputs | Reported PLCC | Train data | Licence |
| --- | --- | --- | --- | --- | --- |
| pVMAF (Synamedia) | not published | QPs + pre-analysis + PSNR | 0.985 (seq) | proprietary | proprietary blog |
| x264-pVMAF (Quortex OSS) | not published | encoder + lightweight pixel | not published seq, SROCC 0.991 frame | proprietary | GPL-2.0 |
| VMAF-E (MainConcept) | not published | not published | "±2 VMAF" → ≈ 0.97 [UNVERIFIED] | proprietary | proprietary |
| **fr_regressor_v2_ensemble (lusoris)** | **~5–20 K × 5** | **6 canonical libvmaf + 18-D codec** | **≥ 0.95 LOSO mean (gate)** | **NF Drop 9×70 + BVI-DVC** | **BSD-3-Clause-Plus-Patent** |

**Gap / opportunity:**

1. We are **slightly behind on raw correlation** versus pVMAF's claimed
   0.985 — but pVMAF's 0.985 is *in-distribution sequence-level on
   proprietary data*, while ours is **LOSO mean** (out-of-source-content),
   a strictly harder gate. A like-for-like comparison would require
   us to publish a sequence-level in-distribution number; per ADR-0303
   we deliberately gate on the harder LOSO number, which is correct
   for a generalisation claim and *underclaims* compared to pVMAF.
2. We should publish a **head-to-head digest vs x264-pVMAF** on the
   Netflix Public Drop (or BVI-DVC) corpus. x264-pVMAF is GPL-2.0 and
   *we cannot link it into our BSD stack*, but we *can* run it as a
   side-by-side benchmark. This would be the single highest-leverage
   external-credibility deliverable for the FR predictor track.
3. Net: our FR predictor is at parity with the published industry SOTA
   on accuracy and **ahead on three procedural dimensions** that no
   competitor publishes — LOSO methodology (ADR-0309), production-
   flip gate (ADR-0303), and probabilistic head with conformal
   prediction (ADR-0291). These are *decision-quality* moats, not
   *number* moats.

### 2. No-reference video quality (NR-VQA)

**What the field is doing:**

- **DOVER (ICCV 2023)** — two-branch (aesthetic + technical), CNN
  backbone; **DOVER-Mobile** uses convnext_v2_femto (inflated), 9.86 M
  params, GFLOPs 52.3, CPU 1.4 s/video. Reported PLCC: KoNViD-1k 0.853,
  LIVE-VQC 0.835, LSVQ_test 0.867, LSVQ_1080p 0.802. Repo
  <https://github.com/VQAssessment/DOVER>. (DOVER full: 200 + MB,
  ~280 GFLOPs, CPU 3.6 s/video; PLCC ~0.88 / 0.85 / 0.89 / 0.83 on the
  same four sets.)
- **FAST-VQA / FasterVQA (ECCV 2022 / TPAMI 2023)** — fragment sampling
  via Grid Mini-patch Sampling; 99.5 % FLOPs reduction at 1080p vs the
  preceding SOTA, fragment-attention transformer. Repo
  <https://github.com/VQAssessment/FAST-VQA-and-FasterVQA>. Still cited
  as a strong tiny-VQA baseline in 2024 challenge tables.
- **Q-Align (ICML 2024)** — LMM-based, "score from text-defined levels".
  SOTA on IQA / VQA / IAA simultaneously; multi-billion-param backbone.
  Not "tiny" by any definition.
- **MaxVQA / Maxwell (ACMMM 2023, MM database, 4 543 in-the-wild
  videos, 13 quality dimensions)** — language-prompted, builds on CLIP.
- **MVQA (April 2025, arXiv 2504.16003)** — Mamba state-space model,
  Unified Sampling, "comparable to SOTA at 2× speed and 1/5 GPU memory".
- **LSVQ** — LIVE Large-Scale Social Video Quality, 39 000 UGC videos,
  5.5 M ratings, *the* canonical large NR-VQA training corpus
  (Hugging Face: `teowu/LSVQ-videos`). Train split 28 056 videos.
- **CHUG (Oct 2025)** — open-call UGC-HDR uploads, 856 sources,
  211 848 AMT ratings, 700 + raters. Public dataset (licence terms
  not yet specified in the paper).
- **NTIRE / AIS challenges 2024–2025** — DOVER fine-tuned on YT-UGC is
  one of the SOTA baselines.

**Where we stand:**

We ship `nr_metric_v1` — a tiny MobileNet-style baseline (~19 K params,
224×224 grayscale single-frame, KoNViD-1k only training, dynamic-PTQ INT8
sidecar, BSD-licensed). **It has not been benchmarked against DOVER /
FAST-VQA / Q-Align on KoNViD / LIVE-VQC / LSVQ.** We do *not* have a
published PLCC vs MOS for our NR head.

In our defence:

- We are ~500× smaller than DOVER-Mobile.
- We run through libvmaf's ONNX Runtime surface, so the same artefact
  serves CPU, CUDA, SYCL, Vulkan and HIP execution providers — runtime
  is whatever the host has, with no separate model build per backend.
- We use 224×224 grayscale single-frame — DOVER uses fragments + temporal.
  These are different design points; the right comparison is NR-VQA
  *deployment cost* (params + FLOPs) at iso-PLCC.

**Gap / opportunity:**

1. **The KonViD-150k Phase-2 corpus expansion** is necessary but
   probably not sufficient. The field's actual large corpus is **LSVQ
   (39 K videos, 5.5 M ratings)**, not KonViD-150k. We should *also*
   ingest LSVQ; it's CC-BY (per Hugging Face card,
   `teowu/LSVQ-videos`) and ~3.7× the ratings of what we're planning.
2. We should run **DOVER-Mobile as an open-weight baseline** in our
   benchmark suite. Hugging Face hosts the weights; the architecture
   is convnext_v2_femto-inflated, ONNX-exportable in principle. This
   gives us a *reproducible* PLCC number to beat.
3. Honest framing: our NR head **today is a placeholder**. The
   `nr_metric_v1` model is a baseline-of-record from 2025; it is not
   a serious answer to DOVER. The user's framing of NR-VQA as
   "in flight via KonViD-150k" is correct — but the destination has
   to be "PLCC ≥ 0.80 on KoNViD-1k *and* LSVQ_test, at < 1 M params,
   under 100 ms CPU/frame", not just "trained on KonViD-150k".

### 3. Codec-aware quality prediction

**What the field is doing:**

- **Per-codec** is the dominant pattern. Synamedia/Quortex publishes
  separate proprietary models for H.264/AVC and H.265/HEVC; the
  open-source x264-pVMAF is *only* x264 (FHD progressive 4:2:0 medium
  preset, no `--tune`). MainConcept VMAF-E does not document codec
  conditioning. Per-title-CRF DNN (Ren et al. 2023) trains for a
  single codec at a time.
- **Codec embedding / one-hot conditioning** as a deliberate
  architectural choice is **not standard** in published VQA work.
  The closest analogue is the AIS 2024 / NTIRE 2024 UGC tracks where
  a single model is *evaluated across codecs* without explicit
  conditioning, treating codec as part of the in-the-wild distribution.
- **Content-adaptive Encoder Preset Prediction** (Telecommunications
  Research Centre Vienna 2022) — content-side codec conditioning,
  reports relative bitrate decrease 17.8 % (x264) and 7.9 % (x265).

**Where we stand:**

ENCODER_VOCAB v3 (16-slot one-hot, ADR-0078 / ADR-0323) lets one model
serve every codec the harness adapter knows about — x264, x265, libaom,
SVT-AV1, libvpx-vp9, vvenc, NVENC variants, QSV variants, VideoToolbox
variants. This is **architecturally cleaner** than the "one model per
codec" industry default but **empirically a wash** on accuracy:
fr_regressor_v3 reports LOSO mean PLCC ≈ 0.9975 (registry note), which
is in the same band as pVMAF's per-codec 0.985 — the codec-conditioning
neither helps nor hurts within measurement noise.

**Gap / opportunity:**

1. The 16-slot vocabulary is **future-proofing for codec churn** (VVC,
   AV2, hardware encoders). The cost is one extra hidden-layer slot
   per codec; the benefit is "every new codec adapter ships with the
   same model checkpoint". This is a *correct* architectural call;
   we should publish it as a positional paper or short tech-note —
   no industry counterpart is on record claiming the same trade-off.
2. Watch-out: per-codec models can specialise on per-codec quirks
   (e.g. x265's psy-rd, libaom's tile-rows) in ways a one-hot model
   can't. We should run **per-codec slice metrics on the BVI-DVC
   evaluation set** to confirm the multi-codec model does not hide
   per-codec underperformance behind an aggregate PLCC.

### 4. Per-shot adaptive encoding with predicted quality

**What the field is doing:**

- **Netflix Per-Title (2018)** and **Per-Shot (2019)** — the founding
  blog posts; Netflix's own pipeline still runs full VMAF, just on
  shot-level segments rather than the whole title.
- **Wu et al. 2022 (arXiv 2208.10739)** — *quality-constant per-shot
  encoding by two-pass learning*. RF-parameter prediction, claims
  98.88 % accuracy of compressed VMAF being within ±1 of target,
  1.55× encoding complexity overhead. **The closest published analogue
  to our predict-then-verify loop.** Two-pass: fast preprocessing →
  feature extraction → DNN-predicted RF → encode.
- **Constructing Per-Shot Bitrate Ladders using VIF (arXiv 2408.01932,
  2024)** — 145-feature VIF set, Extra-Trees regressor, BVT-100 4K
  corpus, 6 resolutions × 16 CRFs, ~20 % bitrate savings vs fixed
  ladder, mean BD-VMAF gain 4.3–4.5. *No reference encoding required
  at inference.*
- **ab-av1, av1an, NETINT capped CRF, MainConcept Constant Target
  Quality** — production tooling, all use full VMAF in a binary
  search rather than predicting CRF directly.
- **rav1e per-frame quality / x265 per-frame VBV / SVT-AV1 adaptive
  temporal filtering** — *encoder-internal* quality knobs, distinct
  from external predicted-VMAF tooling.

**Where we stand:**

`vmaf-tune predict` + `vmaf-tune fast` (per-shot path) implements the
*third* published variant of this idea, with two distinguishing
features:

- **Predict-then-verify loop with explicit verdicts** —
  `GOSPEL / RECALIBRATE / FALL_BACK` per ADR-0067 / ADR-0276 /
  predictor_validate.py. Wu et al. 2022 has a two-pass loop but does
  not publish a *verdict semantic*; the field does not have a
  language for "the predictor was confident-and-right vs
  confident-and-wrong vs uncertain-don't-trust-it".
- **Per-codec MLP shipped alongside the whole-fork ensemble** — the
  per-codec `predictor_<codec>.onnx` files in our model registry are
  *separate* from `fr_regressor_v2_ensemble`. The ensemble is the
  *cross-validation gate* for the whole stack; the per-codec MLPs are
  the *runtime fast path*. No published competitor has this two-tier
  shape.

**Gap / opportunity:**

1. The **GOSPEL/RECALIBRATE/FALL_BACK verdict semantic** is a
   publishable abstraction — write it up as a short arXiv preprint
   or a fork-public blog post. We already have ADR-0276, but the
   field does not have a *named* convention for this. First-mover
   on terminology has compounding citation value.
2. Compare **VIFF9 (Wu/Bovik 2024) vs our `ShotFeatures`** — they
   use 145 features, we use ~16. Ablation: does dropping our 16 to 8
   meaningfully change LOSO PLCC, and would 145 close the gap to
   their 0.762 PLCC on cross-over bitrate? *(This is a benchmark we
   could run; their corpus BVT-100 is publicly available.)*
3. The Wu et al. 2022 2-pass loop's 98.88 % @ ±1 VMAF is a stronger
   target than anything we publish for the per-shot path. We should
   regenerate this number on our predictor + verify pipeline.

### 5. Saliency-aware ROI encoding

**What the field is doing:**

- **DUTS-TR (2017)** — image-level saliency, used by us for
  `saliency_student_v1` (ADR-0270). Not video-temporal.
- **ViNet-S / ViNet-A (ICASSP 2025)** — DHF1K-trained video saliency,
  ViNet-S 36 MB / 1000+ FPS / U-Net decoder, ViNet-A 148 MB with
  spatio-temporal action localisation. Ensemble achieves SOTA on
  three visual + six audio-visual saliency datasets.
- **SalFoM (Springer 2024)** — UnMasked Teacher backbone +
  spatio-temporal locality decoder; SOTA on DHF1K + Hollywood-2 + UCF
  Sports.
- **DHF1K (CVPR 2018, PAMI 2019)** — 1 K eye-tracked video sequences,
  the canonical video saliency benchmark.
- **Encoder-integrated saliency**:
  - **x264 + saliency map** (msu-video-group/x264_saliency_mod) — fork
    of x264 that takes external saliency maps as input. Active.
  - **x265 visual-attention-guided AQ** (KTH thesis 2023, integrated
    into x265, A/B tested on SVT Play). HEVC.
  - **AV1**: no published saliency-ROI integration in mainline
    SVT-AV1; SVT-AV1-PSY (the perceptual fork) was **archived
    2025-04-20**, with juliobbv-p/svt-av1-hdr the recommended
    continuation. **The AV1 ecosystem just lost its psy-tuned fork.**
- **Saliency-guided pre-processing** (Stanford CS231n 2022) —
  preprocessing rather than encoder integration.

**Where we stand:**

`saliency_student_v1` (~113 K params, fork-trained on DUTS-TR,
ADR-0270) drives the QP-offset map for x264 in our `vmaf-tune saliency`
path. This is **architecturally on par with the x265-visual-attention
KTH thesis** — same shape (saliency model → QP-offset → encoder), just
on x264. We are *static-frame* saliency, not temporal; ViNet-S /
SalFoM exist and are bigger but get temporal coherence we lack.

**Gap / opportunity:**

1. **Switch to ViNet-S as the saliency teacher** (or run a temporal
   distillation from ViNet-S into a fork-trained tiny temporal
   student). DUTS-TR-trained static saliency systematically *flickers*
   on real video — every published video-saliency method, including
   ViNet, beats DUTS-TR-only baselines on temporal-coherence
   benchmarks. ViNet-S is 36 MB; a distilled fork-trained student
   can be much smaller. Repo: <https://github.com/ViNet-Saliency/vinet_v2>.
2. **Extend saliency-ROI to x265 / SVT-AV1**. The x264 path is solid;
   x265 has `--zones` we could drive directly (the KTH thesis already
   wired this). SVT-AV1 has no public saliency-ROI integration since
   the PSY fork archived; **a fork-public AV1 saliency-ROI path
   would be a genuine first**.
3. The user's claim "our saliency path is competitive" is correct
   for x264 today and *uniquely correct* for AV1 if we ship the AV1
   integration in 2026.

### 6. Online / sidecar learning for video quality

**What the field is doing:**

- **Continual-learning literature** (CORE 2024, SuRe 2025, Manifold
  Expansion Replay 2023) — replay buffers, EWC, surprise-driven
  prioritised replay. **No published video-quality application**
  found in the search.
- **Federated learning for video streaming** (Springer Nature 2025
  chapter "Federated Learning for Scalable Video Streaming") — focuses
  on bitrate adaptation / personalised content recommendation, not
  on training quality predictors federatedly.
- **Personalised quality models** — there is academic work on
  personalised quality preference (per-user MOS regression) but no
  shipped production system found.
- **Catastrophic forgetting in tiny models** — explicit research
  area; Schick et al. 2024 (ResearchGate 381882542) shows model size
  affects forgetting in nuanced ways, smaller models forget *less*
  on simple tasks but *more* on long task chains.

**Where we stand:**

The local-sidecar bias-correction scaffold (today's commit) is
**ahead of any published prior art for VQ specifically**. The pattern
"shipped predictor + sidecar additive-bias term that learns from a
local user's encode results" exists nowhere I can find in the VQ
literature. The closest analogues are personalised-recommendation
systems (Spotify, YouTube) that update user-specific embeddings on
the device — but those are *recommendation*, not *quality
prediction*.

**Gap / opportunity:**

1. We should **document the sidecar pattern in a tech-note** before
   shipping it. ADR-0042 covers per-PR docs; a research digest *of
   the pattern* is the right place to claim novelty.
2. **Catastrophic forgetting is a real risk** if the sidecar bias
   term is ever fed back into the shipped predictor. Industry
   prescription: replay buffer of "anchor" examples (NF Public Drop
   VI-DVC) interleaved with new data. We should design this
   *before* the sidecar pattern moves from scaffold to closed loop.
3. **Federated VQ training** (the user's K>10-contributor vision) is
   still genuinely open ground — no published precedent. Do not
   build the infrastructure yet (cost > benefit at K=1), but write
   the ADR specifying the *trigger conditions* for when we would.

### 7. Training corpora — UGC vs cinematic balance

**What the field is doing:**

| Corpus | Content | Size | Ratings | Used for |
| --- | --- | --- | --- | --- |
| KoNViD-1k | UGC | 1 200 | ~50 / video | NR-VQA training, validation |
| KoNViD-150k (planned by us) | UGC | 150 000 | crowd | NR-VQA at scale |
| **LSVQ** | **UGC** | **39 075** | **5.5 M** | **NR-VQA SOTA training** |
| YT-UGC | UGC | 1 380 | crowd | NR-VQA validation |
| LIVE-VQC | UGC | 585 | crowd | NR-VQA validation |
| Maxwell / MaxVQA | UGC | 4 543 | 2 M, 13 dimensions | explainable NR-VQA |
| BVI-DVC | cinematic | 200 ref × 18 dist | encoded | FR-VQA training, ours |
| BVI-UGC (2024) | UGC transcoded | 60 ref × 18 dist | 3 500 raters | UGC-transcode VQA |
| Netflix Public Drop | cinematic | 9 ref × 70 dist | encoded | FR-VQA training, ours |
| CHUG (2025) | UGC-HDR | 856 | 211 848 | HDR-VQA |
| LEHA-CVQAD (2025) | compressed | n/a | n/a | compressed-VQA |

**Where we stand:**

For FR we are well-served (NF Public Drop + BVI-DVC). For NR we are
critically *under-served* — KoNViD-1k alone, with KonViD-150k pending.
The field's flagship NR corpus is **LSVQ**, not KonViD-150k.

**Gap / opportunity:**

1. **Ingest LSVQ in parallel with KonViD-150k**, not after. LSVQ has
   ~3.7× more ratings than the proposed KonViD-150k workflow and is
   the de facto SOTA-comparison corpus. Hugging Face hosts it
   (`teowu/LSVQ-videos`).
2. **Maxwell (4 543 videos × 13 quality dimensions)** is the right
   corpus if we ever ship an *explainable* NR-VQA head — i.e. one
   that says "this video is quality 65; the dominant degradation is
   motion blur". Worth a feasibility ADR.
3. CHUG (HDR-UGC, 856 videos) is the right corpus for an HDR-NR head;
   pairs with our existing HDR work in `vmaf-tune hdr`.

### 8. Tiny-model deployment — ONNX runtime patterns

**What the field is doing:**

- **ONNX Runtime quantization** — INT8 (S8S8 with QDQ default), FP16,
  dynamic range optimisation, supports U8U8/U8S8/S8S8.
- **Selective Quantization Tuning** (arXiv 2507.12196, July 2025) —
  per-op, per-device quantization tuning is an active research area.
- **Quantization Robustness for Object Detection** (May 2025, arXiv
  2508.19600) — empirical study of FP32 / FP16 / Dynamic UINT8 / Static
  INT8 across YOLO models, real-world degradation robustness.
- **Industry default for video models**: FP16 dominates GPU paths
  (Tensor-core support); INT8 for CPU/edge. Mixed-precision is the
  emerging frontier (NVIDIA NVFP4 2026 report).
- **ONNX op allowlist as a deployment standard**: not industry-
  standard. Most projects ship ONNX with whatever ops the exporter
  produces; the *allowlist* discipline is rare outside safety-critical
  ML.
- **Sigstore signing for ML model artefacts**: the
  **sigstore/model-transparency** project (v1.1.1, Oct 2025) is the
  canonical infra; Red Hat published "Model authenticity and
  transparency with Sigstore" (April 2025). The pattern is **emerging,
  not yet standard**. No specific public adopters listed in the
  model-transparency README.

**Where we stand:**

Our stack ships **all five disciplines** simultaneously:

- ONNX op allowlist (per-model; CI gate).
- Per-model PTQ accuracy budget (`quant_accuracy_budget_plcc`, e.g.
  0.01 for `learned_filter_v1` and `nr_metric_v1`).
- Dynamic INT8 sidecar for size-sensitive deployments.
- Sigstore keyless signing with bundled `*.sigstore.json` per model.
- ONNX opset pinning at 17 (per-model record).

This is **ahead of every public open-source VQA project** I could find.
DOVER ships PyTorch checkpoints, no ONNX, no signing. FAST-VQA same.
Q-Align same. The **commercial counterparts (pVMAF, VMAF-E)
presumably have signing/quant pipelines internally but do not publish
them**.

**Gap / opportunity:**

1. We have a **publishable practice paper** here. Not a research paper —
   a "how a small VQA project ships ONNX with the full
   supply-chain stack" tech-note. There is no public counterpart.
2. **FP16 vs INT8 study on our models** — we currently ship dynamic
   INT8 for some (`learned_filter_v1`, `nr_metric_v1`); an FP16 path
   would benefit GPU-deployed users (whoever embeds libvmaf in an
   ONNX-Runtime CUDA / DirectML pipeline). Cost: small ablation.
3. The op allowlist is *strict* — confirm via CI gate that we have
   not silently regressed against the published allowlist when we
   add new model types (transformer / Mamba / attention-heavy NR
   head).

### 9. Ensemble-with-gating

**What the field is doing:**

- **Deep ensembles** — well-established (Lakshminarayanan 2017);
  median pooling and weighted averaging both reported as performance
  improvements in NTIRE 2024 short-form UGC-VQA challenge.
- **Stochastic Weight Averaging (SWA) and EMA** — used by NTIRE 2024
  top-team SJTU MMLab for training stabilisation; PLCC + SROCC > 0.9
  reported.
- **Conformal prediction** — distribution-free uncertainty
  quantification; ACM Computing Surveys 2025 calls it a "data-perspective"
  framework. **No published video-quality assessment paper using
  conformal prediction** in our search. There is general work on CP
  for image segmentation (MICCAI 2025 paper 3902), CP for NLP
  (TACL 2024), CP under adversarial attack (VRCP 2025). The video
  quality field has not adopted it.
- **Production gates**: NTIRE / AIS challenges report "best-of-validation"
  weights; no two-criterion (mean + spread) gate published.

**Where we stand:**

Two genuinely-novel patterns:

1. **ADR-0303 production-flip gate** — flips the registry's
   "production model" pointer only when *both* mean LOSO PLCC ≥ 0.95
   *and* max-min LOSO PLCC spread ≤ 0.005. The two-criterion shape
   ("ensemble is good *and* tight") is **not in the published
   literature** I could find; competitors gate on a single metric.
2. **ADR-0291 probabilistic head with conformal calibration** —
   conformal prediction *for VQA* is genuinely first-of-its-kind in the
   public literature. The field uses Bayesian / MC-dropout / deep
   ensembles for uncertainty, **not** conformal coverage.

**Gap / opportunity:**

1. **Publish the production-flip gate as a short tech-note**. The
   shape is publishable, the math is trivial, and naming first wins
   citations.
2. **Publish the conformal-prediction-for-VQA result** as an arXiv
   preprint. We have a working implementation; the field has a literal
   gap. Estimated effort: 2–4 weeks. ROI: high (first arXiv preprint
   that cites lusoris's ADR system; compounding).
3. Watch-out: the production-flip gate's thresholds (0.95, 0.005) are
   set by hand. A Bayesian or empirical-Bayes calibration of these
   thresholds — derived from a held-out gate-failure rate target —
   would be a stronger positional claim.

### 10. The community-data-loop

**What the field is doing:**

- **CHUG (2025)** — open-call UGC-HDR uploads with consent + AMT
  ratings. **The only published open-call UGC dataset I could find
  in the 2024–2026 search.** Not a closed feedback loop — the
  contributors uploaded, the dataset was built once, AMT rated it.
- **YouTube UGC dataset** — sampled from CC-licensed uploads, not a
  feedback-loop community.
- **Crowdsourced subjective tests** (arXiv 2509.20118, Sept 2025) —
  comparative methodology study. Standard AMT-style worker recruitment.
- **Federated VQ training** — *no public production system found*.
- **Personalised-quality models** — academic only.
- **Anonymisation patterns for video metadata** — generic privacy /
  GDPR literature; no VQ-specific best practice published.

**Where we stand:**

The user's vision — community-uploaded encode results contributing
to training — is **genuinely novel ground**. The closest published
analogue is CHUG, which is a one-shot open-call dataset, not a
continuous feedback loop. Federated VQ training does not have
production prior art. The user's "the real cheese" instinct is
correct: this direction is *open*, in the strong sense of "no one
has published it".

**Gap / opportunity:**

1. **Do not build the infrastructure yet.** At K = 1 contributor
   (the user themselves), the cost of a federation infrastructure
   is enormously larger than the benefit of the data. Defer until
   K ≥ 10.
2. **Do build the data-format and consent-and-licence ADRs now.**
   The hardest part of a community data loop is not the federation
   software — it's the rights-transfer agreements, anonymisation,
   redistribution licence. CHUG handled this with *individual*
   "rights transfer agreements"; we should specify ours *before*
   accepting first contribution. Trigger condition: we should not
   accept community data for training without the legal scaffolding
   in place, even from K = 1.
3. **Position-paper the pattern** — write a short ADR (or tech-note)
   describing the pattern at the abstraction level "shipped
   predictor + community-uploaded encode-result deltas → updated
   sidecar bias term". This pre-claims the novelty without
   committing to the federated infrastructure.

---

## High-impact next moves (ranked, for the project's planning round)

1. **Run an external benchmark vs x264-pVMAF (Quortex OSS) and
   DOVER-Mobile** on the BVI-DVC + Netflix Public Drop test splits.
   Publish the digest as `docs/research/00NN-fr-and-nr-external-
   benchmarks.md`. *Effort: 3–5 dev-days. Leverage: this is the
   single most credibility-defining external comparison the project
   has not yet done.* Touches: `tools/vmaf-tune/`, `ai/scripts/
   eval_loso_*.py`, possibly a new
   `ai/scripts/external_benchmark_pvmaf.py`.
2. **Write the ADR-0303 production-flip-gate + ADR-0291 conformal-VQA
   short tech-note for arXiv**, citing the ADRs by number. The math
   is settled; the field-side novelty-claim is real. *Effort:
   2–4 weeks. Leverage: first arXiv preprint that names lusoris's
   ADR system; compounding citation value.* Output: `docs/papers/
   tech-note-production-flip-gate.md` (tracked source) + arXiv submission.
3. **Ingest LSVQ alongside KonViD-150k** (CC-BY licence, Hugging Face
   hosted, ~3.7× ratings of KonViD-150k). *Effort: 2–3 dev-days
   (download tooling + corpus ingestion). Leverage: makes our NR
   numbers comparable to DOVER / FAST-VQA / Q-Align.* Touches: a new
   `ai/scripts/fetch_lsvq.py` + corpus-ingestion ADR.
4. **Ship saliency-ROI for AV1** (SVT-AV1 segment map, since
   SVT-AV1-PSY is archived). *Effort: 1–2 weeks. Leverage: be the
   first fork-public AV1 saliency-ROI integration on the internet.*
   Touches: `tools/vmaf-tune/src/vmaftune/codec_adapters/svtav1.py`,
   new ADR.
5. **Distil ViNet-S into a fork-trained tiny temporal saliency
   student**, replacing DUTS-TR-trained `saliency_student_v1`. ViNet-S
   is 36 MB; a distilled student can be ~150 K params. *Effort:
   2–3 weeks. Leverage: temporal coherence on real video, the missing
   axis from our static saliency story.* Touches:
   `ai/scripts/train_saliency_student.py`, new model-card.
6. **Document the predict-then-verify GOSPEL/RECALIBRATE/FALL_BACK
   verdict semantic** as a publishable abstraction. The pattern is
   already in ADR-0276; promoting it to a *named* convention has
   compounding value. *Effort: 2–3 dev-days for the write-up.*
7. **Write the community-data-loop legal-scaffold ADR before
   accepting any external contribution**, even from K = 1. Specify
   consent format, redistribution licence, anonymisation,
   rights-transfer agreement. *Effort: 1 week. Leverage: cannot
   safely accept *any* external contribution without this.* Touches:
   `docs/adr/0NNN-community-data-loop-legal-scaffold.md`.

---

## Discarded directions (with reason)

- **Skip federated learning infrastructure for now** — at K = 1
  contributor (the user) the cost of a federation stack is several
  orders of magnitude larger than the benefit of the data. Revisit
  at K ≥ 10. *Source*: ACM Queue federated-privacy survey 2024 +
  general cost-of-infra reasoning.
- **Skip Q-Align / LMM-based VQA as a *deployment* target** — Q-Align
  is multi-billion params and depends on a vision-language foundation
  model. Even Q-Align's *evaluation* requires a GPU. Outside our
  "tiny" envelope. Worth tracking as a benchmark *teacher* (knowledge
  distillation from Q-Align into our tiny student is a possible
  research direction at K ≥ 100), not as a deployment artefact.
- **Skip Mamba state-space NR-VQA (MVQA, Q-Mamba)** for now —
  the speed-up vs DOVER is real but Mamba ops are not in our ONNX
  allowlist and are not stable across ONNX Runtime versions. Revisit
  in 2027 once SSM ops stabilise.
- **Skip DOVER-as-teacher distillation** until we have an LSVQ-
  ingested corpus to train against. Distilling DOVER on KoNViD-1k
  alone would just inherit DOVER's KoNViD-1k overfit.
- **Skip x264-pVMAF as a code dependency** — GPL-2.0 is incompatible
  with our BSD-3-Clause-Plus-Patent stack. We can run it as an
  external benchmark but not link or vendor it.
- **Skip SVT-AV1-PSY as an upstream-track target** — archived
  2025-04-20, no future. Track svt-av1-hdr if the perceptual story
  matters for AV1.
- **Skip Huawei PyTorch-VMAF** as a build-time dependency — code not
  released as of latest report. Re-check in 6 months.

---

## References

Numbered citation list. WebSearch / WebFetch dates are for *2026-05-08*
unless noted.

1. Synamedia. *Real-Time Video Quality Assessment with pVMAF*. Blog post.
   <https://www.synamedia.com/blog/real-time-video-quality-assessment-with-pvmaf/>
   — pVMAF architecture (1–2-hidden-layer MLP), inputs (QPs +
   pre-analysis + PSNR), accuracy (PLCC 0.985, SROCC 0.988 sequence-
   level), CPU overhead (~0.06 %). WebFetch 2026-05-08.
2. Synamedia / Quortex. *Unlocking Real-Time Video Quality Measurement
   with x264-pVMAF*. Blog post (2024-11-03).
   <https://www.synamedia.com/blog/unlocking-real-time-video-quality-measurement-with-x264-pvmaf/>
   — open-source x264-pVMAF (35× faster than VMAF, frame-level SROCC
   0.991, FHD progressive 4:2:0 medium-preset). WebFetch 2026-05-08.
3. Quortex. *x264-pVMAF*. GitHub repository.
   <https://github.com/quortex/x264-pVMAF> — GPL-2.0; inference-only;
   no training code; SIMD-optimised C; 3 214 commits at WebFetch.
   WebFetch 2026-05-08.
4. MainConcept. *VMAF-E*. Product page.
   <https://www.mainconcept.com/vmaf-e> — neural-net VMAF
   approximator; ±2 VMAF; 10× faster than VMAF; in-coding-order
   integration. WebSearch 2026-05-08.
5. MainConcept. *vScore and VMAF-E (IBC 2025)*. Press release
   (Sept 2025). <https://www.mainconcept.com/ibc2025-vscore-vmafe>.
6. Anastasia Antsiferova et al. *Hacking VMAF and VMAF NEG*.
   Semantic Scholar paper id 71c676b4ec1465ed6a52684c1cf5ffea7a717c45.
   <https://www.semanticscholar.org/paper/71c676b4ec1465ed6a52684c1cf5ffea7a717c45>.
   *(Cited only for context; the search hit, not a primary
   distillation result.)*
7. Cloud BU, Huawei Technologies. *VMAF Re-implementation on PyTorch:
   Some Experimental Results* (Sept 2023, latest revision Dec 2023).
   arXiv 2310.15578.
   <https://arxiv.org/html/2310.15578v3> — full pipeline reproduction;
   ≲ 10⁻² VMAF unit discrepancy; gradient-based; code release deferred
   for security review. WebFetch 2026-05-08.
8. Wu, Haoning et al. *Exploring Video Quality Assessment on User
   Generated Contents from Aesthetic and Technical Perspectives*
   (DOVER, ICCV 2023). arXiv 2211.04894.
   <https://arxiv.org/abs/2211.04894>. Repo
   <https://github.com/VQAssessment/DOVER>. DOVER-Mobile architecture
   convnext_v2_femto-inflated, 9.86 M params, GFLOPs 52.3, PLCC
   KoNViD-1k 0.853 / LSVQ_test 0.867. WebSearch 2026-05-08.
9. Wu, Haoning et al. *FAST-VQA: Efficient End-to-End Video Quality
   Assessment with Fragment Sampling* (ECCV 2022, TPAMI 2023).
   arXiv 2207.02595. <https://arxiv.org/abs/2207.02595>. Repo
   <https://github.com/VQAssessment/FAST-VQA-and-FasterVQA>.
   WebSearch 2026-05-08.
10. Wu, Haoning et al. *Q-Align: Teaching LMMs for Visual Scoring via
    Discrete Text-Defined Levels* (ICML 2024). Q-Future GitHub
    organisation <https://github.com/Q-Future>. WebSearch 2026-05-08.
11. Wu, Haoning et al. *Towards Explainable In-the-Wild Video Quality
    Assessment: A Database and a Language-Prompted Approach* (Maxwell
    database, ACMMM 2023). arXiv 2305.12726.
    <https://arxiv.org/abs/2305.12726>. WebSearch 2026-05-08.
12. Ying et al. *Patch-VQA: 'Patching Up' the Video Quality Problem*
    (LSVQ database). Repo <https://github.com/baidut/PatchVQ>.
    Hugging Face dataset card `teowu/LSVQ-videos`. WebSearch
    2026-05-08.
13. Saini, Shreshth et al. *CHUG: Crowdsourced User-Generated HDR
    Video Quality Dataset* (Oct 2025). arXiv 2510.09879.
    <https://arxiv.org/html/2510.09879>. 856 sources, 211 848 AMT
    ratings, 700+ raters, open-call uploads with rights-transfer
    agreements. WebFetch 2026-05-08.
14. Wu, Hao et al. *MVQA: Mamba with Unified Sampling for Efficient
    Video Quality Assessment* (April 2025). arXiv 2504.16003.
    <https://arxiv.org/abs/2504.16003>. WebSearch 2026-05-08.
15. Y. Sang et al. *Constructing Per-Shot Bitrate Ladders using
    Visual Information Fidelity* (Aug 2024). arXiv 2408.01932.
    <https://arxiv.org/html/2408.01932>. Extra-Trees regressor,
    BVT-100 4K corpus, 6 res × 16 CRFs, ~20 % bitrate savings.
    WebFetch 2026-05-08.
16. Wu, Tianyu et al. *Quality-Constant Per-Shot Encoding by Two-Pass
    Learning-Based Rate Factor Prediction* (2022). arXiv 2208.10739.
    <https://arxiv.org/pdf/2208.10739>. WebSearch 2026-05-08.
17. Ren et al. *Per-title and per-segment CRF estimation using DNNs
    for quality-based video coding* (Expert Systems with Applications
    2023). doi 10.1016/j.eswa.2023.120469.
    <https://www.sciencedirect.com/science/article/pii/S0957417423007911>.
    WebFetch 2026-05-08.
18. KTH Royal Institute of Technology. *Visual Attention Guided
    Adaptive Quantization for x265* (thesis, 2023).
    <https://kth.diva-portal.org/smash/get/diva2:1788172/FULLTEXT01.pdf>.
    WebSearch 2026-05-08.
19. MSU Video Group. *x264_saliency_mod* (x264 with custom-saliency-
    map input). <https://github.com/msu-video-group/x264_saliency_mod>.
    WebSearch 2026-05-08.
20. ViNet-Saliency. *vinet_v2 (ICASSP 2025)*. GitHub
    <https://github.com/ViNet-Saliency/vinet_v2>. ViNet-S 36 MB,
    1000+ FPS, U-Net decoder; ViNet-A 148 MB. WebSearch 2026-05-08.
21. Wang, Wenguan et al. *Revisiting Video Saliency: A Large-scale
    Benchmark and a New Model* (DHF1K, CVPR 2018, PAMI 2019).
    <https://github.com/wenguanwang/DHF1K>. WebSearch 2026-05-08.
22. SalFoM. *Dynamic Saliency Prediction with Video Foundation
    Models* (Springer 2024).
    <https://link.springer.com/chapter/10.1007/978-3-031-78312-8_3>.
    WebSearch 2026-05-08.
23. SVT-AV1-PSY (archived 2025-04-20). README final state.
    <https://github.com/psy-ex/svt-av1-psy/blob/master/README_old.md>.
    Recommended successor: juliobbv-p/svt-av1-hdr. WebFetch
    2026-05-08.
24. ONNX Runtime. *Quantize ONNX Models* (current docs).
    <https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html>.
    WebSearch 2026-05-08.
25. Selective Quantization Tuning for ONNX Models (July 2025).
    arXiv 2507.12196. <https://arxiv.org/html/2507.12196v1>.
    WebSearch 2026-05-08.
26. Sigstore. *model-transparency*. GitHub
    <https://github.com/sigstore/model-transparency>. v1.1.1
    (Oct 2025), 232 stars. WebFetch 2026-05-08.
27. Red Hat Emerging Technologies. *Model authenticity and
    transparency with Sigstore* (April 2025).
    <https://next.redhat.com/2025/04/10/model-authenticity-and-transparency-with-sigstore/>.
    WebSearch 2026-05-08.
28. NTIRE 2024 Challenge on Short-form UGC VQA: Methods and Results.
    arXiv 2404.11313.
    <https://arxiv.org/html/2404.11313v1>. SWA + EMA strategies, top-team
    SJTU MMLab PLCC + SROCC > 0.9. WebSearch 2026-05-08.
29. ACM Computing Surveys. *Conformal Prediction: A Data Perspective*
    (2025). <https://dl.acm.org/doi/10.1145/3736575>. WebSearch
    2026-05-08.
30. ResearchGate publication 381882542. *The impact of model size on
    catastrophic forgetting in Online Continual Learning*.
    WebSearch 2026-05-08.
31. CORE: *Mitigating Catastrophic Forgetting in Continual Learning
    through Cognitive Replay* (2024). arXiv 2402.01348.
    <https://arxiv.org/html/2402.01348v1>. WebSearch 2026-05-08.
32. Springer Nature. *Federated Learning for Scalable Video
    Streaming* (2025).
    <https://link.springer.com/chapter/10.1007/978-3-031-84651-9_3>.
    WebSearch 2026-05-08.
33. Xinyi Wang et al. *BVI-UGC: A Video Quality Database for
    User-Generated Content Transcoding* (Aug 2024). arXiv 2408.07171.
    <https://arxiv.org/html/2408.07171v1>. 60 ref × 18 dist, 3 500
    raters. WebSearch 2026-05-08.
34. Wang Yutian et al. *Comparative Study of Subjective Video Quality
    Assessment Test Methods in Crowdsourcing* (Sept 2025).
    arXiv 2509.20118. <https://arxiv.org/abs/2509.20118>. WebSearch
    2026-05-08.
35. NVIDIA Technical Blog. *Calculating Video Quality Using NVIDIA
    GPUs and VMAF-CUDA*.
    <https://developer.nvidia.com/blog/calculating-video-quality-using-nvidia-gpus-and-vmaf-cuda/>.
    WebSearch 2026-05-08.
36. ScienceDirect, Memory-VQA: *Video quality assessment of UGC
    based on human memory system* (2025). doi via journal.
    WebSearch 2026-05-08.
37. Lakshminarayanan, Pritzel & Blundell. *Simple and Scalable
    Predictive Uncertainty Estimation using Deep Ensembles* (2017).
    *Cited as the deep-ensemble baseline reference*; not separately
    WebFetch'd.

---

## Process notes

- This digest is research-only; no code or tests are changed by the
  PR that lands it.
- Every quantitative claim about an external system (PLCC, SROCC,
  param count, GFLOPs, dataset size, model architecture) is sourced
  from a WebSearch result or a WebFetch'd primary source on
  2026-05-08. Where a number could not be confirmed, the entry is
  tagged `[UNVERIFIED]`.
- The "TL;DR" three "novel" claims (predict-then-verify verdict
  semantic, ADR-0303 production-flip gate, conformal-VQA) are
  *negative-search* claims — i.e. I searched and found no published
  prior art. A future reader should treat this as "no obvious public
  precedent at write-time" rather than "provably first".
- Pairs with the existing planning ADRs ADR-0303 / ADR-0291 / ADR-0309 /
  ADR-0310; does not itself create an ADR. Concrete next-move bullets
  in §"High-impact next moves" are candidates for separate ADRs.
