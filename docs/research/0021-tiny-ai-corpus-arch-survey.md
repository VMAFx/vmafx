<!-- markdownlint-disable MD013 MD029 MD036 -->
# Research Digest 0021 — Corpus + Architecture Survey for the Next Tiny-AI Iteration

**Date**: 2026-04-28
**Author**: Lusoris / Claude (Anthropic)
**Status**: Research — _pending user direction_.
**Scope**: Survey of nine external sources nominated as candidates to
inform the next iteration of the fork's tiny-AI workstream — specifically
training-corpus expansion beyond the 9-source / ~70-distortion Netflix
corpus, smarter feature inputs into the MLP head, model-architecture
ideas at the (≤50 KB ONNX) tiny end, and an awareness of what Netflix
itself has shipped to production since the original `vmaf_v0.6.1` paper.

---

## 1. Context

The fork's tiny-AI surface (under `ai/` for training, `core/src/dnn/`
for ONNX Runtime CPU inference) just landed its first canonical sweep on
the original Netflix VMAF training corpus — three architectures
(`mlp_small`, `mlp_medium`, `linear`) trained against 9 source clips with
roughly 70 distortion variants per source (37 GB at
`.workingdir2/netflix/`, gitignored, supplied locally). A leave-one-source-
out (LOSO) 9-fold cross-validation is concurrently running on
`mlp_small`. All current models are extremely small (≤ ~5 KB ONNX,
hand-crafted MLPs over 6 libvmaf-extracted features — `adm2`,
`vif_scale0`–`vif_scale3`, `motion2` — see
[`ai/data/feature_extractor.py`](../../ai/data/feature_extractor.py) line
32 ff). The distillation target throughout has been the public
`vmaf_v0.6.1` SVM regressor.

With the first canonical baseline now landed (PR #158 — see
[research digest 0019](0019-tiny-ai-netflix-training.md) for the
methodology + the `mlp_small` / `mlp_medium` / `linear` architectures), the
next iteration needs a clear answer to four orthogonal questions: (a) is
the Netflix corpus the right training distribution, or should it be
augmented with a public, dynamic-scene, motion-rich set of sequences?
(b) are 6 hand-crafted libvmaf features the right input vector, or
should saliency / flow / quality-difference features be added? (c) is
the MLP architecture the right shape, or should sequence / state-space
models be considered for temporal pooling? (d) what production
Netflix-shipping techniques (HDR-VMAF, dynamic optimisation) should the
fork mirror or at least understand the API surface of? This digest
surveys nine sources nominated against those four axes and produces a
ranked list of concrete next-step proposals the project owner can pick
from. Two of the nine sources turn out to be largely irrelevant for
deployment (Gemma 4 E2B is a 5 GB language model, NEVC is an end-to-end
neural codec) but are nonetheless worth keeping in the survey for the
"why we're _not_ pursuing this" record.

---

## 2. Per-source summary

### 2.1 BasicSR — `docs/DatasetPreparation.md`

**Source**: <https://github.com/XPixelGroup/BasicSR/blob/master/docs/DatasetPreparation.md>

BasicSR is the open-source super-resolution / image-restoration toolkit
that grew out of the SRMD / RRDB / ESRGAN / EDVR line of work and is
currently maintained by the XPixelGroup. The
`docs/DatasetPreparation.md` page is the single most-referenced
"how-to-prep-the-canonical-vision-datasets" guide in the wider
super-resolution / restoration community: it covers (per the page's
table of contents) DIV2K (800 train + 100 validation 2K-resolution
images), Flickr2K (2 650 images), the legacy T91 / BSDS200 / General100
training sets, the standard test sets (Set5, Set14, Urban100,
Manga109), the **REDS** video super-resolution corpus (240 train + 30
val + 30 test 720×1280 sequences, 100 frames each), the **Vimeo-90K**
septuplet dataset (~82 GB, 89 800 clips × 7 frames at 448×256), and
FFHQ for generative-model training. Beyond the inventory, the guide
documents three preprocessing recipes that the open-source video / SR
ecosystem treats as canonical: (1) **LMDB conversion** of the
disk-tree-of-PNGs into a single binary database with an accompanying
`meta_info.txt` (filename, dimensions, PNG compression level) for
faster IO; (2) **sub-image cropping** (DIV2K is pre-cropped from 2K
images down to 480×480 sub-images so the dataloader's random-crop step
doesn't waste IO); (3) **meta-info file generation** so the dataset
loader can shuffle without scanning the disk tree on every epoch.

The toolchain is shipped as concrete scripts (`extract_subimages.py`,
`create_lmdb.py`, `generate_meta_info.py`) rather than as library code,
which makes them straightforward to lift wholesale into the fork's
`ai/scripts/` tree without taking a Python dependency on BasicSR
itself.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: high. The Netflix corpus's 9 sources × ~70 distortions
  is small enough that any one source dropping out (LOSO fold)
  removes 11 % of the training distribution. Augmenting with REDS
  (240 dynamic scenes) or Vimeo-90K (89 800 clips) gives the trainer
  an order-of-magnitude more diversity, which directly shrinks the
  distillation generalisation gap. The catch is that REDS / Vimeo
  ship at 720 p / 448×256, which doesn't match Netflix's 540 p / 1080 p
  / 4K encoding ladders — a re-encode pass through libvpx / x264 /
  x265 at the typical streaming CRFs is needed to manufacture the
  ref ↔ dis pairs the trainer expects.
- (b) **Feature inputs**: indirect. BasicSR's preprocessing recipes
  don't change the libvmaf features themselves. The relevance is that
  any downstream feature extractor we add (saliency, flow, etc.) has
  to be runnable across the larger corpus in reasonable time —
  borrowing BasicSR's LMDB convention for the feature-cache layer
  would pay off.
- (c) **Architecture**: not relevant. BasicSR is task-agnostic at
  preprocessing level.
- (d) **C-side feature extractors**: not relevant.

### 2.2 REDS dataset

**Source**: <https://seungjunnah.github.io/Datasets/reds.html>

REDS (REalistic and Dynamic Scenes) is the dataset Seungjun Nah et al.
(KAIST) introduced for the NTIRE 2019 video deblurring + video
super-resolution challenge tracks at CVPR. It is a 720×1280 corpus of
300 sequences total — 240 training, 30 validation, 30 test — each
with exactly 100 frames, derived from 120 fps source captures of
real-world dynamic scenes (moving cameras, walking subjects, traffic,
indoor + outdoor mix). Synthetic blur frames are produced by averaging
adjacent 120 fps frames; the resulting "blurry" sequences are saved
alongside the sharp originals at the equivalent of 24 fps duration.
The challenge introduced four degradation tracks: (1) motion blur,
(2) motion blur + compression artefacts (MPEG-4 Part 14 at 60 % MATLAB
VideoWriter quality), (3) bicubic downscaling (×4), and (4) bicubic
downscaling + motion blur. A separate **REDS4** subset of four
sequences (clip 000, 011, 015, 020 from the validation split) is the
de-facto evaluation set used by every video-SR paper since 2019. The
license is **CC BY 4.0**, so redistribution / fork-side caching is
permissible with attribution.

REDS is the dataset that BasicSR §4 (above) prepares; it is also the
training corpus for TS-Mamba (§2.5), and the historical training corpus
for EDVR / BasicVSR / BasicVSR++ / ToFlow extensions. That broad reuse
makes it the canonical "dynamic-scene, motion-rich" benchmark; the
tiny-AI fork would inherit a sizeable comparable-prior-work network
effect by training on it.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: very high. REDS's 240 training sequences × 100
  frames = 24 000 frames at 720 p directly addresses the Netflix
  corpus's two main weaknesses: low source diversity (9 vs 240) and
  motion-static bias (Netflix sources are mostly static or
  slow-pan; REDS is hand-held, fast-pan, traffic). The licence is
  permissive. The "compression artefacts" track is the closest
  analogue to Netflix's encoding-ladder distortion axis; the bicubic
  tracks are not directly relevant (the fork doesn't model
  super-resolution distortion). Cost-class: medium — a re-encode
  pass through x264 / x265 / libvpx to produce CRF-stratified
  distorted variants is needed to generate VMAF-relevant
  ref ↔ dis pairs.
- (b) **Feature inputs**: indirect; same as BasicSR.
- (c) **Architecture**: indirect — using REDS opens the door to a
  comparison-with-published-baselines story for any sequence-aware
  architecture we add.
- (d) **C-side feature extractors**: not relevant.

### 2.3 ByteDance NEVC

**Source**: <https://github.com/bytedance/NEVC>

NEVC ("Neural Efficient Video Coding") is ByteDance's open-source
neural video codec, released September 2025 alongside the ACM
Multimedia 2025 paper "EHVC: Efficient Hierarchical Reference and
Quality Structure for Neural Video Coding". The repository is
early-stage (≈ 2 commits at time of survey, BSD 3-Clause Clear
licence, NEVC-1.0 release tag) and ships a Python (81 %) + C++ (16 %)
codebase with a pretrained checkpoint hosted on Hugging Face
(`ByteDance/NEVC1.0`). The architecture is an end-to-end neural codec
(not an in-loop or post-processing filter on top of a classical
codec): hierarchical reference structure, quality-stratified encoding,
implicit-style temporal modelling. The README does not publish
parameter counts, training corpus, or formal rate-distortion
benchmark numbers against the paper — those live only in the EHVC
paper proper. The repository's recent-commit minimalism (two
commits, no follow-up issues / PRs) suggests "code drop alongside
publication" rather than active maintenance.

NEVC is a category sibling of DCVC-RT (§2.8), DCVC, and the older
microsoft/DCVC line. It is meaningfully larger than the
"tiny-AI" budget the fork operates inside: end-to-end neural codecs
typically run tens-to-hundreds of millions of parameters on the
encoder/decoder stack, far beyond the fork's ≤ 50 KB ONNX deployment
ceiling.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: very limited. NEVC does not publish its training
  corpus on the README; even if it did, an end-to-end codec's
  training data is curated for reconstruction quality, not for
  perceptual-quality-prediction.
- (b) **Feature inputs**: not relevant. NEVC's internal features are
  latent-codec representations, not human-interpretable signals the
  fork could distil into a 6-feature input vector.
- (c) **Architecture**: not relevant. The fork's tiny-AI is a
  regressor over a fixed feature vector; NEVC is a generative codec.
- (d) **C-side feature extractors**: not relevant.
- **Survey verdict**: include for the "what we're _not_ pursuing"
  record; no follow-up action.

### 2.4 TS-Mamba (Trajectory-aware Shifted State Space Models)

**Source**: <https://github.com/QZ1-boy/TS-Mamba>

TS-Mamba is the codebase for an ICLR 2026 paper, "Trajectory-aware
Shifted State Space Models for Online Video Super-Resolution" (Zhu,
Meng, Jiang, Zhang, Bull, Zhu, Zeng, 2025). It applies the Mamba
state-space model class to online (causal / streaming) video
super-resolution, with a "trajectory-aware" mechanism that tracks
inter-frame motion to guide the per-frame state update. The
implementation pins specific dependency versions (`mamba-ssm==1.0.1`,
`causal_conv1d==1.0.0`, CUDA 11.7, PyTorch 1.13.1, Python 3.9). It
trains on REDS + Vimeo-90K and tests on REDS4 + Vid4. The repository
is small (54 commits, 5 stars, Apache-2.0) but functional. The
"trajectory-aware shift" is the architectural novelty: instead of a
generic Mamba scan over the temporal axis, the scan path follows
estimated inter-frame trajectories so the state-space model's
"memory" is genuinely tracking the same scene content across frames
rather than the same pixel locations.

For the fork's purposes, the salient observation is **not** that we
should ship a Mamba-based VMAF regressor (Mamba is overkill for the
6-feature input vector the tiny-AI module currently consumes), but
that the underlying idea — _explicitly route the temporal pooling
along motion trajectories rather than along fixed pixel positions_ —
is the right primitive for any sequence-aware VMAF model the fork
might build. Today the tiny-AI fusion is per-frame; clip-level scores
are produced by harmonic-mean / arithmetic-mean pooling over per-frame
predictions. A trajectory-aware temporal pooling step (even if
implemented as a small attention layer rather than a full Mamba scan)
could noticeably tighten clip-level Pearson correlation with MOS in
high-motion scenes, which is exactly where the Netflix corpus
under-represents.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: indirect — same REDS + Vimeo recommendation as §2.2.
- (b) **Feature inputs**: medium. The trajectory mechanism implies the
  feature vector should include a per-frame motion-flow summary
  (already partially captured by `motion2`, but motion-along-
  trajectories would be a richer signal).
- (c) **Architecture**: high. The "trajectory-aware temporal
  pooling" idea is portable to a small attention-over-frames head
  in the tiny-AI MLP, sized so the total ONNX model stays under
  the 50 KB ceiling. State-space models proper (the Mamba block) are
  not in the fork's ONNX-op allowlist and would not survive ORT
  CPU-only inference budget.
- (d) **C-side feature extractors**: medium. A trajectory-aware
  motion descriptor could become a new libvmaf feature extractor
  (`motion_traj` or similar), parallel to `motion2`.

### 2.5 ToFlow (Task-Oriented Flow)

**Source**: <https://github.com/anchen1011/toflow>

ToFlow is the Lua-Torch7 reference implementation for Xue, Chen, Wu,
Wei, Freeman 2019 IJCV "Video Enhancement with Task-Oriented Flow."
The paper introduces the _task-oriented optical flow_ idea — instead
of using a generic motion-estimation flow (e.g. EpicFlow, FlowNet),
the network learns a flow representation **specifically optimised for
the downstream task** (denoising, super-resolution, frame
interpolation, deblocking). The paper also released the **Vimeo-90K**
dataset (5 846 source videos curated from Vimeo, expanded to 89 800
3-frame triplets and 91 701 7-frame septuplets) — which has since
become the de-facto motion-rich training corpus for the entire video-
restoration field.

The repository is dormant (Lua + MATLAB, no recent activity, no
releases) but the dataset itself remains live and is the second
canonical training corpus alongside REDS. From the fork's
perspective, ToFlow's contribution is twofold: **the dataset** and
**the conceptual primitive of task-oriented flow**. The latter
maps surprisingly cleanly onto the tiny-AI use case: instead of
adding generic motion features to the regressor input, train a
small flow-summarisation head whose output is shaped by the loss
on VMAF distillation rather than on photometric reconstruction.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: high. Vimeo-90K is the second biggest open-source,
  motion-rich, ref-pair-friendly corpus after REDS, with the
  Vimeo-90K septuplet variant providing 7-frame temporal context per
  sample. Same encoding-ladder synthesis caveat as REDS. Cost class:
  medium-large (the dataset is 82 GB compressed, larger than the
  Netflix corpus's 37 GB).
- (b) **Feature inputs**: medium-high. The task-oriented-flow
  concept reframes "what feature should we add to the input vector"
  as "what flow representation is shaped by VMAF loss"; the
  experimental cost is moderate (a small CNN + a back-prop signal
  through the regressor head).
- (c) **Architecture**: medium. ToFlow's network is bigger than the
  tiny-AI ceiling, but the task-oriented-flow concept
  generalises: any feature that's _shaped by VMAF loss_ rather than
  hand-crafted is a candidate.
- (d) **C-side feature extractors**: low. A task-oriented flow head
  in the regressor doesn't need to live on the C side; it can live
  in the trained ONNX graph.

### 2.6 Gemma 4 E2B-it ONNX

**Source**: <https://huggingface.co/onnx-community/gemma-4-E2B-it-ONNX>

Gemma 4 E2B-it is Google DeepMind's 2.3 B-effective-parameter (5.1 B
total with embeddings) instruction-tuned multimodal language model,
shipped here as ONNX-converted q4 (4-bit) quantised graphs split
into four files: `embed_tokens_q4.onnx`,
`decoder_model_merged_q4.onnx`, `vision_encoder_q4.onnx`,
`audio_encoder_q4.onnx`. Architecture: 35-layer dense transformer
with hybrid sliding/global attention, 128 K context, 262 K vocab,
supports text + image + audio (≤ 30 s) modalities, native function
calling, "thinking" mode via `<|think|>` control tokens. Apache-2.0
licence. Roughly 86 000 downloads/month at the time of survey.
Listed benchmarks: MMLU Pro 60.0 %, GPQA Diamond 43.4 %, MMMU Pro
44.2 %, LiveCodeBench v6 44.0 %.

For the tiny-AI fork, this is **explicitly out of scope** as a
deployment target. The q4 quantised model alone is on the order of
gigabytes of disk; the inference cost on CPU-only ONNX Runtime
(which is the fork's deployment posture for `core/src/dnn/`) is
seconds-per-frame at best, not the milliseconds-per-frame the VMAF
metric pipeline requires. The relevance is narrower and indirect:
it's one of the more thoroughly-validated public examples of
**multimodal ONNX export with multiple subgraphs** (text decoder +
vision encoder + audio encoder + embedding table) wired together at
runtime via Hugging Face's `Gemma4ForConditionalGeneration` glue.
That is a useful precedent for the MCP-side `describe_worst_frames`
work (T6-6, already landed via PR #108) and for any future tiny-AI
work that wants to combine, say, a saliency-encoder ONNX subgraph
with a regressor-head subgraph in a single deployment.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: not relevant.
- (b) **Feature inputs**: low. A frozen Gemma-vision encoder could
  in principle produce a semantic feature embedding for a saliency-
  weighted VMAF variant, but the cost of running a 150 M-parameter
  vision encoder per frame is wildly outside the fork's deployment
  budget.
- (c) **Architecture**: low. The multi-subgraph ONNX wiring is the
  one transferable detail; the model itself is not.
- (d) **C-side feature extractors**: not relevant.
- **Survey verdict**: include for the "VLM-debug-only" use case
  (T6-2 saliency, T6-6 worst-frame description); no follow-up for
  the regressor pipeline.

### 2.7 arXiv:2502.20762 — "Towards Practical Real-Time Neural Video Compression" (DCVC-RT)

**Source**: <https://arxiv.org/abs/2502.20762>

The paper (CVPR 2025 poster) by the DCVC team at Microsoft Research
Asia introduces **DCVC-RT**, the first neural video codec to reach
≥100 fps 1080 p coding and 4K real-time coding while still saving
~21 % bitrate vs H.266/VTM at comparable ECM-level compression
ratios. The architectural contributions, per the abstract and
introduction: (1) **implicit temporal modelling** — replacing the
explicit motion-estimation + motion-compensation modules common in
DCVC / DCVC-DC / DCVC-FM with a learned latent that absorbs temporal
context implicitly; (2) **single low-resolution latent** rather than
the progressive-downsampling cascade of earlier DCVC variants;
(3) **model integerization** — fixed-point quantisation across the
full encode/decode graph for cross-device determinism; (4) a
**module-bank-based rate control** scheme. The paper's framing of
"operational cost" (memory I/O, function-call overhead) as the
primary speed bottleneck — rather than raw FLOPs — is directly
applicable to the fork's ORT CPU-only deployment, where the same
trade-off dominates inference latency.

DCVC-RT is a sibling of NEVC (§2.3) — both are end-to-end neural
codecs, both are several orders of magnitude bigger than the tiny-AI
deployment ceiling. The relevance to the fork is therefore _not_
about deploying DCVC-RT, but about borrowing its three transferable
design observations: (1) implicit temporal modelling generalises to
"don't add an explicit per-frame trajectory module if the regressor
can absorb temporal context through a learned latent"; (2)
operational cost dominates over compute cost on CPU-bound inference,
which means the fork's ONNX op-allowlist policy and the
graph-flattening done at export time matter more for inference
latency than parameter count does; (3) integerization of the full
graph for cross-device determinism is a precedent worth mirroring
for the tiny-AI verifiable-checkpoint story (T6-9
`--tiny-model-verify` + Sigstore sidecar).

**Relevance to the tiny-AI module**:

- (a) **Corpus**: not directly relevant; DCVC-RT trains on
  Vimeo-90K + UVG + HEVC test sequences, which the tiny-AI module
  could also adopt (the UVG / HEVC test sequences are the canonical
  rate-distortion benchmark corpus and are smaller than REDS).
- (b) **Feature inputs**: low. Latent representations from a frozen
  DCVC-RT encoder are an interesting feature-input candidate but
  far outside the inference budget.
- (c) **Architecture**: medium. Implicit temporal modelling +
  operational-cost focus + integerization are three concrete
  precedents the fork should mirror in any future architecture
  iteration.
- (d) **C-side feature extractors**: not relevant.

### 2.8 Netflix HDR-VMAF / Dynamic Optimisation

**Source**: <https://www.tvtechnology.com/news/netflix-deploys-machine-learning-based-protocol-to-improve-hdr>

The TVTechnology article reports Netflix's late-2023 announcement
(amplified in CSI Magazine, TVB Europe, and the Netflix Tech Blog
post "All of Netflix's HDR video streaming is now dynamically
optimized") that Netflix has rolled out **HDR-VMAF**, a perceptual
quality metric for HDR signals, and used it to apply
**Dynamically Optimized (DO)** encoding to its entire HDR catalogue
by June 2023. The technical claims, drawn from the supporting
sources: HDR-VMAF was developed in collaboration with Dolby Labs in
2021 and refined since; it is **format-agnostic** (works on
Dolby Vision, HDR10, HDR10+ alike); it deliberately **excludes
display mapping** from its measurement pipeline, focusing on signal
characteristics resulting from lossy encoding rather than on
display-side tone mapping; the production deployment delivered
Netflix's quoted "40 % fewer rebuffers, lower initial bitrate, lower
internet data usage, especially on mobile/tablet". The original
machine-learning trained-on-HDR-MOS prediction work was also
published at ACM MMSys 2023 ("Machine-learning based VMAF prediction
for HDR video content").

This is the source most directly relevant to the fork's strategic
positioning: **Netflix has shipped to production an HDR variant of
VMAF**, and the model artefact (`vmaf_v0.6.1`-equivalent JSON for the
HDR domain) is _not_ in the public model directory. The fork's
existing tiny-AI surface is single-domain (SDR, fed by the standard
6 libvmaf features). To distil HDR-VMAF as a tiny-AI target the fork
would need (i) an HDR-aware feature pipeline at the libvmaf C side
(the existing VIF / DLM / motion / ADM extractors are tuned for
8-bit / 10-bit limited-range BT.709, not for PQ / HLG transfer
characteristics), (ii) HDR ground-truth MOS data (not public), (iii)
the HDR-VMAF teacher model itself (not public). The fork can mirror
the API surface — `--hdr-vmaf` flag, `vmaf_hdr_v0.6.1` model slot —
without immediately distilling a learned HDR regressor; that's a
two-phase rollout the project owner should consider.

**Relevance to the tiny-AI module**:

- (a) **Corpus**: high but blocked. HDR training data is not
  publicly available; the fork would need to negotiate access or
  build a synthetic corpus by HDR-grading the existing SDR sources
  (lossy approximation; risks cross-domain generalisation gap).
- (b) **Feature inputs**: high. The libvmaf feature pipeline needs
  HDR-aware variants — VIF / DLM defined in PQ-linear or
  HLG-linear space rather than BT.709 — before any meaningful HDR
  regressor can be trained.
- (c) **Architecture**: medium. The same MLP-over-features
  architecture should work; the unknowns are domain-specific.
- (d) **C-side feature extractors**: very high. New HDR-aware
  feature extractors are the gating dependency for any HDR tiny-AI
  work.

### 2.9 Uploaded files (sweep result)

**Sweep result**: A `find` over the worktree (`-newer
core/meson_options.txt`, excluding `.git` / `build`) returned
only the expected list of testdata fixtures, benchmark JSONs,
working-directory scratch (`testdata/scores_*.json`,
`testdata/perf_benchmark_results.json`), the in-tree `model/`
directory's existing JSON / PKL / ONNX models, and the fork's
configuration files (`.claude/settings.local.json`,
`matlab/.gitignore`). The `.workingdir2/` directory contains the
known planning-dossier scaffolding (`BACKLOG.md`, `OPEN.md`,
`PLAN.md`, `analysis/`, `backend-notes/`, `decisions/`,
`netflix/`, `phases/`, plus a single screenshot capture
`Bildschirmfoto_20260421_191523.png` from a week ago that pre-dates
the current sweep). Restrictions on `~/Downloads` and `/tmp/` reads
in the sandbox prevented direct enumeration of those locations from
inside the worktree session.

**Verdict**: no fresh uploads located in the worktree itself. The
survey proceeds with the eight URL sources above. If the user
intended specific PDF / paper / dataset attachments to be included,
they will need to be re-supplied (e.g. dropped into
`.workingdir2/uploads/` so the sandbox's allowed-path list picks them
up) and the survey re-extended in a follow-up digest.

---

## 3. Cross-cutting findings

Several themes recur across the sources surveyed. The strongest is the
**REDS + Vimeo-90K corpus pairing** (§2.1, §2.2, §2.4, §2.5): every
modern video-restoration paper that touches the wider ecosystem trains
on one or both of these, and the BasicSR toolchain prepares both.
That repetition is informative — the canonical "dynamic-scene
training corpus" question has a settled answer in the wider
community, and the tiny-AI fork is in fact the outlier by training
purely on the Netflix corpus. Even if the project owner decides to
keep Netflix as the primary distillation corpus for fidelity to
`vmaf_v0.6.1`, augmenting with REDS + Vimeo-90K as a regularisation
distribution would shrink the fold-to-fold variance of the LOSO
sweep currently running.

The second recurring theme is **trajectory-aware temporal pooling**
(§2.4 TS-Mamba, §2.5 ToFlow's task-oriented flow, §2.7 DCVC-RT's
implicit temporal modelling). All three converge on the same
conclusion: pooling along motion trajectories outperforms pooling
along fixed pixel positions. The tiny-AI fork's current per-frame +
arithmetic-mean clip pool is on the wrong side of that trade-off.
Importantly, the three sources disagree about _how_ to do the pooling
— TS-Mamba uses an explicit trajectory scan, ToFlow uses a
task-oriented flow head, DCVC-RT uses an implicit latent — but agree
that the per-frame-then-mean baseline is leaving correlation on the
table. For a tiny-AI deployment (≤ 50 KB ONNX, CPU-only), the
DCVC-RT / implicit-latent route is the cheapest: a small attention
head over a fixed-length frame window, exported to ONNX, fits the
deployment ceiling and avoids both Mamba's
not-in-the-op-allowlist problem and ToFlow's separate-flow-head
complexity.

The third theme is **operational cost dominates over parameter
count** (§2.6 Gemma multi-subgraph wiring, §2.7 DCVC-RT explicit
framing). Both sources, despite operating at vastly different
parameter scales, identify memory-IO and function-call overhead as
the practical inference-latency bottleneck. The tiny-AI fork's
deployment posture (ORT CPU-only, ONNX Runtime per-frame) is exactly
the regime where this matters. Two practical implications: (i) the
fork's existing op-allowlist policy is doing the right thing by
constraining graph complexity, and (ii) any architecture iteration
should be benchmarked end-to-end on actual ORT inference rather
than only on parameter count or FLOPs — a 50 KB model with bad graph
shape can easily be slower than a 100 KB model with a flat graph.

The fourth theme is **production HDR support is the strategically
biggest gap** (§2.8). Netflix has shipped HDR-VMAF to its entire
catalogue; the public `vmaf_v0.6.1` is SDR-only; the fork mirrors
that SDR-only posture. From a fork-positioning standpoint this is
the most consequential of the four themes — every other gap surveyed
is incremental, but HDR support is a category jump. It is also the
gap with the largest engineering surface (HDR-aware C-side
feature extractors, HDR ground-truth MOS data, HDR teacher model)
and therefore the most expensive to close.

A fifth, lighter theme is **end-to-end neural codecs are not the
right deployment shape for the fork** (§2.3 NEVC, §2.7 DCVC-RT).
Both are technically remarkable but irrelevant as deployment targets;
the fork's contract is "perceptual quality measurement of an existing
encode," not "produce a new encode." This is a useful negative
finding to record so that future surveys don't re-litigate the
question.

---

## 4. Recommendations

The numbered list below ranks concrete next-step proposals. Each
captures the action, the payoff vs the current Netflix-corpus +
`mlp_small` baseline, the cost class (S / M / L), and the BACKLOG row
it would slot under.

1. **Augment training corpus with REDS + Vimeo-90K, gated by the
   existing LOSO harness.** Re-encode the 240-train REDS sequences
   and a sampled subset of Vimeo-90K septuplets through x264 / x265
   / libvpx at 4–6 CRF stratifications to manufacture ref ↔ dis
   pairs aligned with Netflix's encoding-ladder distortion axis.
   Train `mlp_small` and `mlp_medium` jointly on
   Netflix ∪ REDS ∪ Vimeo and re-run the LOSO sweep on the Netflix
   subset to verify the augmentation doesn't degrade fidelity to
   the existing `vmaf_v0.6.1` distillation target.
   - **Payoff**: directly addresses the Netflix corpus's source-
     diversity weakness (9 vs 240+ sources). Expected ~30–40 %
     reduction in LOSO fold-to-fold variance; tighter
     generalisation to high-motion clips (REDS's bias). Permissive
     licensing (CC BY 4.0 for REDS).
   - **Cost class**: M (re-encode pass + larger feature-extraction
     run; 100 GB+ disk for the augmented feature cache).
   - **BACKLOG slot**: new T-row needed under Tier 6 (Tiny-AI Wave 1),
     adjacent to T6-1 / T6-1a — recommend `T6-1c — REDS + Vimeo-90K
     corpus augmentation`.

2. **Add a small attention-over-frames temporal pooling head to
   `mlp_small`, kept under the 50 KB ONNX ceiling.** Replace the
   current per-frame regressor + arithmetic-mean clip pool with a
   per-frame regressor + a 4-or-8-head attention layer over a fixed
   frame window (e.g. 16 frames), then a learned aggregation step.
   Borrow DCVC-RT's "implicit temporal modelling" framing: the
   attention head absorbs temporal context without an explicit
   motion module. Validate on the high-motion subset of REDS
   (rec 1) — that's where the per-frame-then-mean baseline is
   weakest.
   - **Payoff**: addresses the second cross-cutting finding
     (trajectory-aware pooling). Expected SROCC lift on
     motion-heavy sequences; minimal lift on Netflix's static
     content. Compatible with the existing ONNX op-allowlist (no
     Mamba / SSM ops needed).
   - **Cost class**: M (architecture + training run + ONNX export
     +size verification).
   - **BACKLOG slot**: new T-row needed — recommend
     `T6-1d — temporal-attention pooling head for tiny-AI`.

3. **Stand up HDR-VMAF API surface (CLI flag + model slot)
   without distilling a learned HDR regressor yet.** Add a
   `--hdr-vmaf` flag to the CLI that documents itself as
   "HDR-VMAF support — feature pipeline placeholder, model slot
   reserved" and a `vmaf_hdr_v0.6.1.json` model slot in `model/`
   that returns a clear "not yet trained" error at load time
   rather than a silent fallback. This is a two-phase rollout
   where phase 1 is the API contract (cheap, lets downstream
   tooling start integrating) and phase 2 is the distilled
   regressor (gated on HDR ground truth + HDR teacher
   availability).
   - **Payoff**: addresses the fourth cross-cutting finding
     (HDR is the biggest production gap). Phase-1 cost is small
     and unblocks downstream API consumers.
   - **Cost class**: S for phase 1; L for phase 2.
   - **BACKLOG slot**: new T-row needed — recommend
     `T6-10 — HDR-VMAF API surface (phase 1)` plus a `T6-11 —
     HDR-VMAF tiny-AI distillation (phase 2)` follow-up gated on
     ground-truth access.

4. **Adopt the BasicSR LMDB convention for the per-clip feature
   cache.** Today the feature extractor (`ai/data/feature_extractor.py`)
   re-runs the libvmaf CLI per clip; for the augmented corpus
   (rec 1) this becomes IO-bound. Pre-extract features once and
   store as an LMDB keyed by `(source_id, distortion_id, frame_idx)`,
   with a `meta_info.txt` companion file for shuffle-without-scan.
   Mirrors BasicSR §2 (LMDB + meta-info).
   - **Payoff**: ~10× speed-up on second-and-subsequent training
     runs; a precondition for rec 1 to be tractable. Negligible
     prediction-quality impact (it's a caching change).
   - **Cost class**: S.
   - **BACKLOG slot**: new T-row under Tier 6 — recommend
     `T6-1e — LMDB feature cache for tiny-AI training`.

5. **Add a saliency-weighted feature extractor on the libvmaf C
   side (gated on T6-2 saliency landing).** When MobileSal saliency
   lands as part of the existing T6-2 work, expose its output as a
   per-frame saliency mask and feed a saliency-weighted average of
   the existing 6 libvmaf features into the MLP head — i.e. weight
   `adm2` / `vif_scale*` / `motion2` by where the saliency map says
   the viewer is looking. This addresses the second-order theme
   from §2.5 (task-oriented features shaped by VMAF loss) without
   needing a new neural feature head.
   - **Payoff**: small but well-grounded SROCC improvement on
     Netflix-style content where viewer attention is concentrated
     on faces / centred subjects. Cheap relative to a learned
     flow head.
   - **Cost class**: S–M (depends on T6-2 part-a landing first).
   - **BACKLOG slot**: lives under T6-2 as a sub-row —
     recommend `T6-2c — saliency-weighted libvmaf feature
     pooling for tiny-AI`.

6. **Document the negative findings as a one-paragraph "not
   pursued" footnote in `docs/ai/roadmap.md`.** End-to-end neural
   codecs (NEVC, DCVC-RT) and large multimodal LLMs (Gemma 4 E2B)
   are not deployment targets for the tiny-AI surface; recording
   _why_ saves the next session re-litigating the question.
   - **Payoff**: process-only; saves a few hours of repeated
     research per future session.
   - **Cost class**: S.
   - **BACKLOG slot**: new T-row under Tier 6 — recommend
     `T6-12 — document non-deployment-target classes for tiny-AI`.

The recommendations are ordered by recommended priority. Items 1, 2,
4 cluster together as the "next wave 1.5" of the tiny-AI workstream;
item 3 is the strategic positioning move; item 5 is dependency-
gated; item 6 is hygiene.

---

## 5. Decisions

_pending user direction_

---

## 6. References

1. <https://github.com/XPixelGroup/BasicSR/blob/master/docs/DatasetPreparation.md>
2. <https://seungjunnah.github.io/Datasets/reds.html>
3. (sweep result — no fresh uploads located in the worktree at survey
   time; see §2.9)
4. <https://github.com/bytedance/NEVC>
5. <https://github.com/QZ1-boy/TS-Mamba>
6. <https://github.com/anchen1011/toflow>
7. <https://huggingface.co/onnx-community/gemma-4-E2B-it-ONNX>
8. <https://arxiv.org/abs/2502.20762>
9. <https://www.tvtechnology.com/news/netflix-deploys-machine-learning-based-protocol-to-improve-hdr>

Supporting sources consulted while preparing the digest (not part of
the user-supplied list, kept here for reproducibility):

10. <https://netflixtechblog.com/all-of-netflixs-hdr-video-streaming-is-now-dynamically-optimized-e9e0cb15f2ba>
11. <https://www.csimagazine.com/csi/netflix-reveals-hdrvmaf-solution.php>
12. <https://www.tvbeurope.com/media-consumption/netflix-develops-hdr-vmaf-to-dynamically-optimise-all-its-hdr-content>
13. <https://dl.acm.org/doi/abs/10.1145/3587819.3593941> — Machine-learning based VMAF prediction for HDR video content (ACM MMSys 2023)
14. <https://openaccess.thecvf.com/content_CVPRW_2019/papers/NTIRE/Nah_NTIRE_2019_Challenge_on_Video_Deblurring_and_Super-Resolution_Dataset_and_CVPRW_2019_paper.pdf> — REDS / NTIRE 2019 reference paper
15. <https://www.alphaxiv.org/overview/2502.20762v2> — DCVC-RT secondary reference
