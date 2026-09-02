<!-- markdownlint-disable MD013 -->
# Research-0086 — Contributor data-pack expansion via web-researched data

- **Status**: Active feasibility study. Pure desk research; no code
  changes proposed in this digest. Follow-up implementation PRs ship
  their own ADRs.
- **Workstream**: ensemble-training-kit contributor pack
  (`tools/ensemble-training-kit/`), tiny-AI corpus row schema
  (`ai/`, `docs/ai/training-data.md`).
- **Last updated**: 2026-05-08
- **Author**: research subagent, prompted by lawrence (paraphrased):
  the contributor pack is missing data about hardware the maintainer
  does not physically own; could that be web-researched and folded
  in? More broadly, what other connected metadata sources could
  give the tiny-AI models a model boost? Acknowledging up-front
  that AI complexity means we cannot predict in advance which
  signals will help — only A/B-test them.

## Question

Two intertwined questions:

1. **Hardware-class coverage gap.** The contributor pack collects
   per-platform corpus rows from contributor machines (CPU, GPU,
   encoder identity, codec build hash, runtime environment). The
   maintainer does not own every relevant hardware class —
   notably Apple Silicon GPUs at every tier, Intel Battlemage / Xe2
   discrete, AMD RDNA4, NVIDIA Blackwell consumer SKUs, mobile
   GPUs (Adreno 8xx, Mali G7xx, Apple A18 Pro), and the modern NPU
   landscape (Intel NPU, Apple Neural Engine, Qualcomm Hexagon,
   AMD XDNA). Can published benchmark numbers, capability
   matrices, and vendor whitepapers fill the gap as a *prior*
   when no contributor for that class has run the kit?

2. **Other "connected" metadata.** Beyond hardware, are there
   public data sources whose rows could plug into the tiny-AI
   training corpus as additional feature columns and produce a
   measurable lift on PLCC / SROCC against the held-out LOSO
   fold? Categories surveyed below: encoder + codec capability
   metadata, public reference VMAF scores, public subjective-
   quality datasets (KonViD-1k / KonViD-150k / LSVQ already on
   the roadmap — what else?), encoder-internal-state signals
   (per-frame stats already produced by every encoder, just not
   captured), and "free" content-class signals (shot similarity
   from TransNet V2, audio loudness from `ffmpeg`, captions /
   OCR for screen content, semantic content classes from a tiny
   CLIP variant).

## Verification posture — read before citing

`[verified]` = directly confirmed against the linked source by a
WebFetch / WebSearch at write time (2026-05-08). `[UNVERIFIED]` =
claim plausible from documentation context but not closeable from
public docs alone in this session's time budget; tagged for
implementation-PR follow-up before any number is shipped.

Per the fork's `feedback_no_guessing` rule: every benchmark number
cited has either an access-dated URL or an `[UNVERIFIED]` tag.
Capability claims (codec-X is supported on hardware-Y) are
verified against primary docs. Specific benchmark *numerical
values* (e.g., "M4 Max = 192 532 Geekbench Metal") are tagged
`[external — secondary aggregator]` and should not be embedded
into the corpus without re-running through the kit on actual
hardware.

## Constraints inherited from CLAUDE.md

- **Netflix golden gate is sacred.** No proposal in this digest
  introduces any change that could perturb the three Netflix CPU
  reference test pairs (`python/test/quality_runner_test.py` et
  al). Web-researched data sits in the *training corpus* for
  the tiny-AI surfaces (FR regressor v2 / ensemble v2), not in
  the libvmaf numerical core. The numerical core stays
  bit-exact.
- **License hygiene first.** Every embedded number must have a
  licence that permits redistribution inside a fork-shipped
  corpus row JSON. Research-only datasets are *referenced* by
  URL and never embedded.
- **Per-surface docs rule.** Any follow-up PR that adds a new
  feature column to the corpus row ships a `docs/ai/` update
  in the same PR (ADR-0042 + ADR-0100).

---

## Category 1 — Hardware-class metadata for hardware we don't own

The contributor pack today emits a per-row hardware fingerprint
(CPU brand, GPU brand, driver version, encoder build hash). Rows
from contributor machines fill the matrix organically. The gap
is hardware classes nobody in the contributor pool has yet
exercised. Two ways to "fill" the gap from web research:

- **(a) Prior-only fill** — register the hardware class with a
  capability fingerprint (codec support matrix, VRAM, fp16/int8
  TOPS) drawn from vendor documentation. No benchmark numbers
  embedded; the class exists as a row template only, with
  `n_rows = 0` until a contributor runs the kit on actual
  hardware. *Information value to tiny-AI: low* — it is a row
  template, not a row.
- **(b) Surrogate-row fill** — synthesise a *predicted* corpus
  row by combining vendor capability docs with public benchmark
  numbers (e.g., "M4 Max ProRes encode @ 4K = N fps") and
  flagging the row as `source: web_surrogate`. *Information
  value: medium-but-risky* — the model can learn from it, but
  if the surrogate is biased the prior leaks into the learnt
  weights. Demands an A/B-ablation gate.

### 1.1 Apple Silicon GPUs (M3 / M4 / M3 Ultra / A18 Pro)

- **Capability matrix source**: Apple Developer documentation,
  per-SoC Metal feature-set tables. *Licensing: Apple
  documentation, fair-use citation, not redistributable as
  bulk corpus rows.* [\[verified — exists at developer.apple.com\]](https://developer.apple.com/metal/)
- **Benchmark numbers**: Geekbench Metal (browser.geekbench.com),
  Notebookcheck per-SoC review pages, MacRumors aggregator.
  Numbers are *secondary*; they reflect the test machine's
  thermal envelope and the OS version. M4 Max Metal score
  ≈ 192 532, M3 Max ≈ 154 860 [\[external — Geekbench browser
  wccftech aggregator, 2026-05-08\]](<https://wccftech.com/m4-max-gpu-benchmarks-revealed/>) — `[UNVERIFIED]` for
  fork-corpus embedding because we cannot replay the run on
  our infrastructure.
- **VMAF-relevant signal**: video-encode capability per Metal
  version (HEVC 422 10-bit, ProRes RAW, AV1 *decode* added
  M4). Encode-side AV1 in Apple Silicon: not supported as of
  M4 / M4 Max [\[verified — Apple developer docs do not list
  AV1 encode in VTCompressionSession capability tables\]](https://developer.apple.com/documentation/videotoolbox).
- **Recommendation**: **EXPERIMENT** with prior-only fill (1a).
  Add a row template per Apple SoC tier (`apple_m3`,
  `apple_m3_pro`, `apple_m3_max`, `apple_m3_ultra`,
  `apple_m4`, `apple_m4_pro`, `apple_m4_max`, `apple_a18_pro`)
  with capability fingerprint only; reject surrogate-row fill
  until a contributor with the hardware runs the kit. Cost:
  ~4 engineer-hours.

### 1.2 Intel Battlemage (Arc B-series, Xe2 dGPU)

- **Capability matrix source**: Intel oneVPL CHANGELOG.md and
  the iHD media-driver release notes. [\[verified — referenced
  from Research-0085, gh-API 2026-05-06\]](https://github.com/oneapi-src/oneVPL/blob/main/CHANGELOG.md)
- **Benchmark numbers**: Phoronix Linux compute coverage for
  B580, GamersNexus and TechPowerUp content-creation reviews.
  H.265 QSV @ 4K reportedly faster than NVENC on RTX 4060
  (12-min clip in 58 s vs 3 min) [\[external — Tom's Hardware
  / TechPowerUp, 2026-05-08\]](https://www.tomshardware.com/pc-components/gpus/intel-arc-b580-review-the-new-usd249-gpu-champion-has-arrived/6) — `[UNVERIFIED]` for
  fork-corpus embedding.
- **VMAF-relevant signal**: encode-time per-codec on hardware
  the fork's vmaf-tune QSV adapter (ADR-0066 series) targets
  but doesn't profile. Battlemage adds no VVC encode (still
  decode-only).
- **Recommendation**: **GO** for prior-only fill (1a). The
  vmaf-tune QSV adapter already references Intel's capability
  matrix indirectly; embedding the matrix as a row template
  removes a class of "missing column" warnings. Cost:
  ~2 engineer-hours, lifts directly from the oneVPL repo.

### 1.3 AMD RDNA4 (RX 9000 series)

- **Capability matrix source**: AMD AMF GitHub release notes
  and the Radeon Open Compute (ROCm) driver matrix. [\[verified
  — referenced from Research-0085\]](https://github.com/GPUOpen-LibrariesAndSDKs/AMF)
- **VMAF-relevant signal**: AV1 encode (Navi 31+ added it;
  Navi 4x retains it), no VVC encode. The fork's HIP backend
  targets RDNA3 / RDNA4 already.
- **Recommendation**: **GO** for prior-only fill (1a). Same
  rationale as Battlemage. Cost: ~2 engineer-hours.

### 1.4 NVIDIA Blackwell consumer SKUs (RTX 5090 / 5080)

- **Capability matrix source**: NVENC Application Note 13.0+
  [\[verified — NVENC 13.0 application note exists at
  docs.nvidia.com\]](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html); supports H.264, HEVC, AV1; Blackwell
  adds 4:2:2 progressive/interlaced encode and AV1 Ultra-High-
  Quality mode [\[verified — NVIDIA dev blog\]](https://developer.nvidia.com/blog/nvidia-video-codec-sdk-13-0-powered-by-nvidia-blackwell/).
- **Recommendation**: **GO** for prior-only fill (1a). Cost:
  ~2 engineer-hours.

### 1.5 Mobile GPUs (Adreno 8xx, Mali G7xx, Apple A18 Pro)

- **Capability matrix source**: Qualcomm Snapdragon developer
  docs (Adreno video core), ARM Mali developer docs (Mali-V83+
  for HEVC encode), Apple A18 Pro VideoToolbox docs.
- **VMAF-relevant signal**: mobile encode is a real fork
  audience (vmaf-tune phone-model fast path); but the
  encoder pipeline behind the SoC is opaque (vendor blob) and
  the per-frame stats this digest's category 5 covers are
  not exposed.
- **Recommendation**: **NO-GO** for prior-only fill — return
  on engineer-hours is low because the corpus row schema's
  encoder-build-hash column cannot be filled (vendor blob).
  Wait for a contributor with mobile hardware to run the kit.

### 1.6 NPUs (Intel NPU, Apple ANE, Qualcomm Hexagon, AMD XDNA)

- The tiny-AI surfaces (FR regressor v2, ensemble v2) currently
  inference on CPU + GPU via ONNX Runtime; NPU paths are not
  on the fork's current roadmap. The corpus row schema does
  not have an NPU column.
- **Recommendation**: **NO-GO** for this round. Re-survey
  when the fork ships an NPU execution provider (no current
  ADR commits to one).

### Category 1 verdict

| Sub-class | Decision | Cost (eng-hours) |
| --- | --- | --- |
| Apple Silicon GPUs | EXPERIMENT (prior-only) | ~4 |
| Intel Battlemage | GO (prior-only) | ~2 |
| AMD RDNA4 | GO (prior-only) | ~2 |
| NVIDIA Blackwell consumer | GO (prior-only) | ~2 |
| Mobile GPUs | NO-GO | — |
| NPUs | NO-GO (premature) | — |

Total category-1 commitment: ~10 engineer-hours, four PRs.
**No surrogate-row fill recommended in any sub-class** — the
A/B-ablation cost to validate a synthesised row exceeds the
information lift.

---

## Category 2 — Public encoder + codec capability metadata

These are vendor release notes and public spec sheets. License-
wise: most are Apache 2.0 (oneVPL, AMF) or MIT (x264 / x265 /
SVT-AV1 changelog files); embedding into a fork-shipped JSON
corpus row is permitted with attribution.

### 2.1 x264 + x265 changelogs

- **Source**: x264 has no canonical changelog file in upstream
  videolan/x264; per-revision history lives across third-party
  trackers (VideoHelp, Digital-Digest, AfterDawn).
  [\[verified — search confirms no canonical CHANGELOG\]](https://www.videohelp.com/software/x264-Encoder/version-history)
  x265 has `x265.readthedocs.io/en/master/releasenotes.html`
  as the canonical source [\[verified\]](https://x265.readthedocs.io/en/master/releasenotes.html).
- **VMAF-relevant signal**: per-version capability fingerprint
  for the corpus row's `encoder_version` column. Today the
  column carries a free-form string; promoting it to a join
  against a known capability table lets the regressor
  generalise across versions.
- **Recommendation**: **GO** for x265 (canonical changelog
  exists). **EXPERIMENT** for x264 (would require parsing the
  videohelp page or running `git log` against the upstream
  videolan/x264 master and tagging every public revision).
  Cost: ~4 engineer-hours for x265, ~10 for x264. Information
  value: medium — encoder version is already a corpus column;
  this enriches it.

### 2.2 SVT-AV1 milestone tables

- **Source**: GitLab AOMediaCodec/SVT-AV1 `CHANGELOG.md`
  [\[verified\]](https://gitlab.com/AOMediaCodec/SVT-AV1/-/blob/master/CHANGELOG.md).
- **License**: BSD-3-Clause (SVT-AV1 LICENSE), redistributable.
- **Recommendation**: **GO**. Cost: ~3 engineer-hours.

### 2.3 libaom + libvvenc CLI option tables

- **Source**: libaom `aom/aomenc.c` argv parsing; libvvenc
  `source/Lib/vvenc/vvenc.cpp` CLI table. Both are
  open-source and parse-able into a JSON capability table.
- **Recommendation**: **EXPERIMENT** — the CLI option space is
  large and rarely the dominant signal. Score it against an
  ablation before committing. Cost: ~6 engineer-hours.

### 2.4 NVENC / AMF / oneVPL capability matrices

Three vendor SDKs, three matrices already covered transitively
under category 1 (sub-classes 1.2 / 1.3 / 1.4). No separate
PR — rolls into the category-1 prior-only fills.

### Category 2 verdict

| Source | Decision | Cost (eng-hours) |
| --- | --- | --- |
| x265 changelog | GO | ~4 |
| SVT-AV1 changelog | GO | ~3 |
| x264 revisions | EXPERIMENT | ~10 |
| libaom / libvvenc CLI | EXPERIMENT | ~6 |

---

## Category 3 — Public reference VMAF scores

Idea: ingest published VMAF numbers from academic papers (per
clip / encoder / CRF) as a *prior* — not as ground truth, but
as a sanity-check signal for the predictor.

### 3.1 What's out there

- BBC R&D BVI-DVC paper publishes VMAF baselines per clip /
  codec / QP (the same corpus that Research-0082 covers from
  the YUV side).
- AOMediaCodec wiki publishes SVT-AV1 vs libaom VMAF deltas
  per preset.
- Netflix techblog "Toward a Practical Perceptual Video Quality
  Metric" (the original VMAF paper) includes per-clip VMAF
  numbers on the Netflix Public corpus.

### 3.2 Risk

Embedding *external* VMAF numbers as features in the *fork's*
predictor risks teaching the model to predict its own output.
The numbers were computed against a specific VMAF model
version (often `vmaf_v0.6.1`); using them as a prior would
hard-code that version as the regressor's reference.

### 3.3 Recommendation

**NO-GO** as a feature column. **EXPERIMENT** as a sanity-check
target — i.e., a held-out *evaluation* set that the regressor
must hit within ε of the published number on a known clip.
This is closer to a regression test than a training signal.
Cost: ~8 engineer-hours to build the eval-only harness; the
information value to *training* is zero by construction.

---

## Category 4 — Public subjective-quality datasets

Beyond KonViD-1k / KonViD-150k / LSVQ already on the radar.

### 4.1 KonViD-150k — already prioritised

- **License**: CC-BY 4.0 [\[verified — search 2026-05-08\]](https://database.mmsp-kn.de/konvid-1k-database.html).
  Redistributable with attribution.
- **Status**: already an open backlog item; this digest does
  not duplicate the existing intake plan.

### 4.2 LSVQ

- **License**: Facebook Research + UT Austin LIVE Lab
  copyright; *not* a Creative Commons / MIT-style permissive
  license. Redistribution requires explicit permission.
  [\[verified — search 2026-05-08\]](https://huggingface.co/datasets/teowu/LSVQ-videos)
- **Recommendation**: **EXPERIMENT** — usable for *local*
  training only (does not enter shipped corpus rows or
  shipped weights). Same redistribution posture as the
  Netflix Public drop and BVI-DVC (Research-0082).

### 4.3 Waterloo IVC family

- **License**: Permissive — "permission is granted without
  written agreement and without license or royalty fees to
  use, copy, modify, and distribute this database and its
  documentation for any purpose, provided that the
  copyright notice ... appear in all copies"
  [\[verified — search 2026-05-08, ivc.uwaterloo.ca\]](https://ivc.uwaterloo.ca/database/).
- **Datasets**: 4K-VQA, 3D Video, Streaming QoE-IV, IVC PVQ.
- **VMAF-relevant signal**: Waterloo 4K-VQA in particular
  fills the 2160p gap that BVI-DVC partly addresses (BVI-DVC
  spans 270p–2160p). Adding 4K-VQA to the LOSO partitions
  expands held-out folds at the highest resolution.
- **Recommendation**: **GO**. License is favourable;
  resolution coverage is a known gap. Cost: ~12 engineer-
  hours (corpus ingestion + LOSO fold expansion + docs/ai/
  update).

### 4.4 YouTube UGC dataset (Wang et al., 2019)

- **License**: Creative Commons (videos sampled from
  CC-licensed YouTube uploads); the dataset itself is
  research-focused. [\[verified — search 2026-05-08\]](https://media.withyoutube.com/)
- **VMAF-relevant signal**: 1500 20-second clips, UGC
  content distribution (gaming, vlog, sports, animation).
  Adds a content-class the existing corpus underweights.
- **Recommendation**: **GO**. Cost: ~16 engineer-hours
  (corpus is large; ingestion is heavier).

### 4.5 Disney Research / Twitch / SHVC-HDR

- Disney has historically published image-quality datasets
  (HDR-image-saliency); video QoE datasets are sparser and
  harder to verify-in-bulk. `[UNVERIFIED]` whether a current
  open Disney *video*-quality corpus exists.
- Twitch has published latency-and-bitrate studies; *open*
  perceptual-quality data is sparser. `[UNVERIFIED]`.
- SHVC-HDR perceptual data is mentioned in academic
  comparison surveys but a single canonical open dataset is
  not obvious. `[UNVERIFIED]`.
- **Recommendation**: **EXPERIMENT** — defer until a follow-
  up research session can verify each candidate dataset's
  license and provenance. Do not commit to ingestion until
  verified.

### Category 4 verdict

| Dataset | License | Decision | Cost (eng-hours) |
| --- | --- | --- | --- |
| KonViD-150k | CC-BY 4.0 | GO (already prioritised) | (existing) |
| LSVQ | restrictive | EXPERIMENT (local only) | ~12 |
| Waterloo IVC 4K-VQA | permissive | GO | ~12 |
| YouTube UGC | CC | GO | ~16 |
| Disney / Twitch / SHVC-HDR | UNVERIFIED | EXPERIMENT (re-survey) | — |

---

## Category 5 — Encoder-internal-state signals

The *highest-leverage / lowest-cost* category in this survey.
When you encode, the encoder emits per-frame stats — rate-
distortion, partition decisions, motion-vector density,
quantiser usage, slice types — and modern encoders write them
to `--stats` / `--pass 1` log files for two-pass rate control.
These signals are *already produced*; the corpus pipeline
just throws them away.

### 5.1 Per-encoder mechanism

- **x264**: `--pass 1 --stats <file>` writes per-frame stats;
  format is documented in `common/mc.h` + `encoder/ratecontrol.c`.
- **x265**: same mechanism, `--pass 1 --stats` flag.
- **SVT-AV1**: IPP first-pass writes per-frame motion-estimation
  statistics; format documented in
  `Docs/Appendix-IPP-Pass.md` and
  `Docs/Appendix-Rate-Control.md`. [\[verified — search
  2026-05-08\]](https://gitlab.com/AOMediaCodec/SVT-AV1/-/blob/master/Docs/Appendix-Rate-Control.md)
- **libaom**: `--pass=1 --fpf=<file>` writes per-frame stats.
- **libvvenc**: `--pass 1` mechanism present.
- **NVENC / AMF / QSV**: vendor SDKs expose per-frame
  rate-control hints via `NV_ENC_LOCK_BITSTREAM` /
  `AMF_VIDEO_ENCODER_OUTPUT_DATA` / `mfxBitstream` extensions.

### 5.2 Schema proposal

Add an optional `encoder_internal` column to the corpus row
schema; column type is opaque JSON keyed by encoder name,
containing whatever `--pass 1` / `--stats` / SDK-extension
emits for that encoder. The tiny-AI feature-engineering layer
extracts a fixed-width vector at training time (per-frame
mean / variance of bits-per-frame, qp, mv-magnitude,
intra-vs-inter ratio, scene-change flag).

### 5.3 Risk

Bias: the per-frame stats reflect the encoder's own quality
heuristic, not human perception. Teaching the regressor too
strongly on encoder-internal signals risks chasing the
encoder's preferences rather than the human-MOS target.
Mitigation: A/B-ablation gate. Train two checkpoints: one
with `encoder_internal` columns, one without. Compare LOSO
PLCC on a held-out content fold the encoder-stats column did
*not* see. If the lift is < 1 PLCC point, do not ship.

### 5.4 Recommendation

**GO** with strict A/B-ablation gate. The per-encoder cost is
~6 engineer-hours per encoder (parse the existing log format,
extract a vector, plumb into the corpus pipeline). Six
encoders × ~6 hours ≈ 36 engineer-hours. Information value:
**high if it works** — the signal is free, captured at
encode time, and orthogonal to current corpus columns.

---

## Category 6 — Other "free" signals

### 6.1 Shot-boundary metadata via TransNet V2

- **Source**: `github.com/soCzech/TransNetV2` — the original
  TF implementation. Apache 2.0 license on the Hugging Face
  weights mirror; MIT license on the PyTorch port via PyPI
  (`transnetv2-pytorch`). [\[verified — search 2026-05-08\]](https://github.com/soCzech/TransNetV2)
- **Use**: emit a `shot_boundary_count`, `mean_shot_length`,
  `max_shot_length` triple per clip. Useful as a content-class
  proxy: cinematic content = few long shots; sports = many
  short shots; UGC = highly variable.
- **Cost**: ~8 engineer-hours (the model is small; ONNX
  port is plausibly already a sibling tiny-AI surface).
- **Recommendation**: **GO**.

### 6.2 Audio loudness via ffmpeg

- **Source**: `ebur128` filter, free with every ffmpeg build.
- **Use**: integrated loudness (LUFS), loudness range (LRA),
  true peak. Sports / talking-head content has predictable
  audio profiles; this is a content-class hint.
- **Cost**: ~2 engineer-hours.
- **Recommendation**: **EXPERIMENT** — low cost but the
  information value is uncertain; A/B-ablate.

### 6.3 Captions / OCR for screen content

- **Source**: tesseract or PaddleOCR on a sampled frame grid.
- **Use**: detect screen-content (presentations, code editors,
  game UI). The fork's screen-content path under vmaf-tune
  could exploit this.
- **Cost**: ~12 engineer-hours; OCR is heavyweight.
- **Recommendation**: **EXPERIMENT** — gate behind a content-
  class detector first, only run OCR on candidate clips.

### 6.4 Semantic content classes via tiny CLIP

- **Source**: a quantised CLIP-tiny variant (e.g.,
  `openai/clip-vit-base-patch32` distilled).
- **Use**: emit a 32-dim semantic embedding per clip, project
  onto a pre-trained content-class taxonomy (animation,
  sports, news, gaming, nature, ...).
- **Cost**: ~16 engineer-hours; non-trivial because the CLIP
  weights and license posture must be re-verified for fork
  redistribution.
- **Recommendation**: **EXPERIMENT** — high information value
  but high cost; defer until cheaper signals (5.1 / 6.1)
  have been ablated.

### Category 6 verdict

| Signal | Decision | Cost (eng-hours) |
| --- | --- | --- |
| Shot boundaries (TransNet V2) | GO | ~8 |
| Audio loudness (ebur128) | EXPERIMENT | ~2 |
| Captions / OCR | EXPERIMENT | ~12 |
| Semantic embedding (CLIP-tiny) | EXPERIMENT | ~16 |

---

## A/B-test design — universal across categories

Every new feature column or new corpus source lands as
**optional and ablate-able**. The training harness gains a
`--feature-set <name>` flag that selects which feature
columns are active. Each follow-up PR ships its proposed
column under a name (e.g., `encoder_internal_v1`,
`shot_boundaries_v1`, `waterloo_ivc_4k`) and lands two
LOSO checkpoints in the PR description:

1. **Baseline**: feature set excludes the new column.
2. **Treatment**: feature set includes the new column.

The PR's gate metric is `delta_PLCC_LOSO ≥ +0.005` (or whatever
the calibration ADR sets) on the held-out fold the new column
did not see at training time. Below the threshold, the column
gets shelved (committed to the feature catalog but disabled
by default).

The maintainer's framing — "AI complexity means we can only
test, not predict in advance" — is the rule, not the
exception. Every column ships with its own ablation
baseline.

---

## Aggregate verdict

| Category | GO | EXPERIMENT | NO-GO |
| --- | --- | --- | --- |
| 1 — Hardware-class metadata | 3 | 1 | 2 |
| 2 — Encoder + codec capability | 2 | 2 | 0 |
| 3 — Public reference VMAF scores | 0 | 1 | 1 |
| 4 — Public subjective datasets | 2 | 2 | 0 |
| 5 — Encoder-internal-state | 1 | 0 | 0 |
| 6 — Other free signals | 1 | 3 | 0 |
| **Total** | **9** | **9** | **3** |

---

## Prioritised follow-up implementation PR list (by leverage / cost)

Ordered by `(estimated_information_value × probability_of_lift) /
estimated_engineer_hours`. Top item is the recommended first
PR.

1. **Encoder-internal-state per-frame stats** (Category 5).
   Cost: ~36 engineer-hours across six encoders, but the
   first-encoder slice (x264 alone) is ~6 hours and validates
   the schema. Information value: high; signal is free at
   encode time. Single-encoder PR-1: ship `encoder_internal_v1`
   with x264 only, gate on `+0.005 PLCC` LOSO ablation.
2. **Waterloo IVC 4K-VQA corpus ingestion** (Category 4.3).
   Cost: ~12 engineer-hours. Permissive license. Fills the
   2160p resolution gap. Information value: medium-high
   (corpus expansion always wins on LOSO if the held-out
   content class was previously missing).
3. **TransNet V2 shot-boundary metadata** (Category 6.1).
   Cost: ~8 engineer-hours. Apache 2.0 license on weights.
   Information value: medium (content-class proxy, orthogonal
   to existing columns).
4. **Hardware capability fingerprints — Battlemage / RDNA4 /
   Blackwell** (Category 1.2 / 1.3 / 1.4). Cost: ~6
   engineer-hours combined. Information value: low for
   training but high for the contributor pack's UX
   (eliminates "missing column" warnings and prepares row
   templates for when contributors with that hardware show
   up).
5. **YouTube UGC corpus ingestion** (Category 4.4). Cost:
   ~16 engineer-hours. CC license. Information value:
   medium-high — adds a UGC content distribution the fork's
   corpus underweights. Larger ingestion cost than Waterloo
   so it's ranked below it on cost-adjusted leverage.

Items 6+ (audio loudness, x265 changelog, SVT-AV1 changelog,
LSVQ local training, Apple Silicon prior-only fills) are
queued as smaller follow-ups.

## What this digest does *not* recommend

- **No surrogate-row fill** for any hardware class — the
  A/B-ablation cost to validate a synthesised row exceeds
  its information lift, and biased priors leak into the
  weights.
- **No external VMAF score embedding as a feature column** —
  the predictor would learn to predict its own output.
- **No NPU pathway** — premature; no fork ADR commits to an
  NPU execution provider yet.
- **No mobile-GPU prior fill** — opaque vendor blob means
  the corpus row's encoder-build-hash column cannot be
  filled; ROI is too low.
- **No claim of bit-exactness change** to the libvmaf
  numerical core. All proposals sit in the *training corpus*
  layer; the Netflix golden gate is untouched.

## References

- ADR-0042 — tiny-AI per-PR docs requirement.
- ADR-0100 — project-wide doc-substance rule.
- ADR-0108 — six deep-dive deliverables on every fork-local
  PR (this digest fulfils item 1 for the workstream).
- ADR-0203 — Netflix Public corpus redistribution posture
  (template for research-only datasets).
- Research-0082 — BVI-DVC corpus feasibility (template for
  corpus-ingestion PRs; used as the reference pattern for
  Waterloo / YouTube UGC ingestion proposals).
- Research-0085 — vendor-neutral VVC encode landscape
  (verification posture template; this digest reuses the
  `[verified]` / `[UNVERIFIED]` discipline).
- `tools/ensemble-training-kit/extract-corpus.sh` and
  `prepare-gdrive-bundle.sh` — the contributor pack
  surfaces this digest expands.
- `docs/ai/training-data.md` — corpus row schema document
  every follow-up PR updates.

User direction (paraphrased per CLAUDE.md user-quote rule):
the maintainer asked whether the contributor pack is missing
data about hardware they don't own and whether web-scraped
data could fill it; broader question on other connected
metadata sources for tiny-AI model boost; explicit framing
that AI complexity means one can only A/B-test, not predict
in advance which signals will help. Captured 2026-05-08.
