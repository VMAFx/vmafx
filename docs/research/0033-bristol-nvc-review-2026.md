<!-- markdownlint-disable MD013 MD060 -->
# Research-0033 — Bristol VI-Lab Neural Video Compression review (2026-04 preprint)

| Field      | Value                                              |
| ---------- | -------------------------------------------------- |
| **Date**   | 2026-05-01                                         |
| **Status** | Audit; actionable items extracted, no code change  |
| **Source** | `.workingdir2/preprints202604.0035.v1.{pdf,html,zip}` (40 pages) |
| **Authors of source** | Gao, Feng, Jiang, Peng, Kwan, Teng, Zeng, Li, Wang, Hamilton, Qi, Zhang, Bull (University of Bristol VI-Lab) |
| **Tags**   | literature, ai, fr-regressor, codec, lpips, dists  |

## What the preprint is

*Advances in Neural Video Compression: A Review and Benchmarking* (Gao
et al. 2026). Taxonomy of scene-agnostic vs scene-adaptive Neural Video
Compression (NVC) across paradigms / backbones / test protocols
(258 references), followed by an empirical BD-rate + complexity
benchmark of DCVC-DC/FM/RT, MaskCRT, PNVC, GIViC, NVRC, HiNeRV, C3
against VTM-20, AV1-3.8.1, ECM-12, AVM-2 on UVG / MCL-JCV / HEVC B-E /
AOM A2-A5 with PSNR, MS-SSIM, **VMAF** as quality metrics.

The paper is *not* about VMAF directly, but uses VMAF as one of three
reporting metrics throughout §6 and lifts several pieces of prior art
that map directly to fork-local surfaces.

## Direct relevance to this fork (ranked by actionability)

### 1. Tiny-AI training corpus expansion — BVI-AOM ingest (1 week + ADR)

**Surface**: `ai/src/vmaf_train/data/datasets.py`,
`docs/ai/training-data.md`. Mirror the existing BVI-DVC manifest.

The paper's Table 2 catalogues canonical NVC training/test sets:
**BVI-DVC** (Ma, Zhang, Bull 2021 — already ingested) and **BVI-AOM**
(Nawała et al. 2024 — *new actionable data we don't ingest*). 956
sequences, AOM-CTC-aligned. Concrete change: add BVI-AOM ingest path
next to the existing BVI-DVC one.

Source: §5.1.1, Table 2, refs [216,217,224].

### 2. NEG-VMAF caveat in `fr_regressor_v1` model card (1 day, no ADR)

**Surface**: `ai/src/vmaf_train/models/fr_regressor.py`,
`docs/ai/models/fr_regressor_v1.md`.

§5.3 cites Siniukov et al. 2021 [238] on "VMAF hacking by pre/post
processing" and Netflix's NEG-VMAF response [235]. Our
`fr_regressor_v1` is trained as a VMAF regressor and inherits this
vulnerability. Concrete change: add a NEG-style adversarial holdout
(sharpened/contrast-boosted distortions) to the eval suite, document
the NEG caveat in the model card.

Source: §5.3, refs [235], [238].

### 3. ST-VMAF / Zhang-2021 prior art for `fr_regressor_v2` design space (1 day digest)

**Surface**: future `fr_regressor_v2`, model registry.

§5.3 cites Bampis et al. 2018 [236] (ST-VMAF, spatiotemporal feature
integration) and Zhang et al. 2021 [237] ("Enhancing VMAF through new
feature integration and model combination", PCS 2021, Bull lab) as
published improvements over stock VMAF. These are direct prior work
on the architectural axis our `fr_regressor` already explores
(motion-aware FR fusion). Read [237], compare its feature set to our
Phase-3 sweep, file a research digest. Multi-week if we re-implement.

Source: §5.3, refs [236,237].

### 4. NVC-style BD-rate report recipe (1 day, doc-only)

**Surface**: `python/vmaf/tools/bd_rate_calculator.py`,
`docs/usage/`.

Tables 3 & 4 use VMAF as a co-equal axis with PSNR and MS-SSIM for
BD-rate against VTM-20 LD/RA. Our `bd_rate_calculator.py` exists; we
lack a documented "NVC-style BD-rate report" recipe that mirrors the
paper's protocol on UVG / MCL-JCV / HEVC B-E so users can reproduce
the standard table.

Source: §5.4, §6.1, Tables 3-4, ref [255] (Bjøntegaard).

### 5. DISTS extractor as LPIPS companion (1 week, ADR)

**Surface**: `core/src/dnn/`, `model/tiny/`.

§5.3 discusses LPIPS [239] *and* DISTS [240, Ding et al. PAMI 2020]
as the deep-feature FR pair widely used for video quality. We ship
`lpips_sq.onnx` but no DISTS extractor. Concrete change: scaffold a
`dists_sq` extractor analogous to `feature_lpips.c`. ONNX export +
op-allowlist check + ADR per ADR-0041 pattern.

Source: §5.3, ref [240].

## Source-list audit

258 references in total. Repo cross-check via `grep -rli` over `docs/`,
`model/`, `ai/`. "already-cited" means the citation key, DOI, or unique
title fragment appears in our tree.

| Ref   | Year | Title (abbrev.)                          | Class                       | Where in our repo |
| ----- | ---- | ---------------------------------------- | --------------------------- | ----------------- |
| [4]   | 2003 | Wiegand — H.264/AVC overview             | already-cited               | `docs/metrics/` (ffmpeg context) |
| [5]   | 2012 | Sullivan — HEVC overview                 | already-cited               | `docs/metrics/ctc/` |
| [6]   | 2021 | Bross — VVC overview                     | already-cited               | `docs/metrics/` |
| [7]   | 2021 | Han — AV1 technical overview             | already-cited               | `docs/metrics/ctc/aom.md` |
| [216] | 2021 | Ma, Zhang, Bull — **BVI-DVC** training database | already-cited        | `docs/research/0019-tiny-ai-netflix-training.md`, `ai/src/vmaf_train/data/datasets.py` |
| [217] | 2024 | Nawała — **BVI-AOM** training dataset    | **directly-relevant + new** | `ai/src/vmaf_train/data/` (ingest path) |
| [219] | 2010 | Bossen — **JVET CTC**                    | already-cited               | `docs/metrics/ctc/` |
| [220] | 2021 | Zhao — **AOM CTC v2**                    | already-cited               | `docs/metrics/ctc/aom.md` |
| [221] | 2020 | Mercat — **UVG** dataset                 | tangential                  | not used as training set |
| [222] | 2016 | Wang — **MCL-JCV** dataset               | tangential                  | not used |
| [225] | 2022 | Zhao — AOM CTC v3                        | already-cited               | `docs/metrics/ctc/aom.md` |
| [226] | 2004 | Wang — **SSIM**                          | already-cited               | `core/src/feature/ssim.c` (origin) |
| [231] | 2006 | Sheikh & Bovik — **VIF**                 | already-cited               | `core/src/feature/vif.c` |
| [234] | 2010 | Seshadrinathan — **MOVIE**               | tangential                  | mentioned only in literature |
| [235] | 2016 | Li — **VMAF (Netflix Tech Blog)**        | already-cited               | `model/vmaf_*.json`, `docs/metrics/vmaf.md` |
| [236] | 2018 | Bampis — **ST-VMAF**                     | **directly-relevant + new** | `ai/src/vmaf_train/models/fr_regressor.py` (motion-aware fusion) |
| [237] | 2021 | Zhang — **Enhancing VMAF** (Bull lab)    | **directly-relevant + new** | `docs/research/` (digest); FR-regressor v2 backlog |
| [238] | 2021 | Siniukov — **VMAF hacking**              | **directly-relevant + new** | `docs/ai/models/fr_regressor_v1.md` (NEG caveat) |
| [239] | 2018 | Zhang — **LPIPS**                        | already-cited               | `model/tiny/lpips_sq.onnx`, ADR-0041 |
| [240] | 2020 | Ding — **DISTS**                         | **directly-relevant + new** | new tiny-AI extractor candidate |
| [241] | 2017 | Liu — **RankIQA**                        | tangential                  | NR-only, future NR-extractor work |
| [242] | 2024 | Feng — **RankDVQA** (Bull lab)           | **directly-relevant + new** | NR-metric backlog |
| [243] | 2025 | Feng — Towards unified VQA               | tangential                  | survey-level |
| [244-254] | 2023-25 | LMM-VQA series (Q-Align / Q-Insight) | tangential              | out of scope for current tiny-AI bar |
| [255] | 2001 | Bjøntegaard — **BD-rate** definition     | already-cited               | `python/vmaf/tools/bd_rate_calculator.py` |
| [257] | 2003 | Wang/Simoncelli/Bovik — **MS-SSIM**      | already-cited               | `core/src/feature/ms_ssim.c`, ADR-0125 |

**Not-relevant count**: ~225 of 258 — entropy-coding theory, NeRF /
Gaussian-splatting variants, end-to-end NVC architectures (DCVC-*,
MaskCRT, GIViC, HiNeRV, NVRC, PNVC), normalizing-flow priors,
vector-quantization variants, LMM-based VQA. These describe full
neural codecs and codec internals — interesting context but no
actionable mapping to a fork-local file. The fork is a quality-metric
library, not a codec.

## Backlog candidates

### Short — `T7-NEG-VMAF` (1 day, no ADR)

**Title**: document NEG / hacking caveat in `fr_regressor_v1` model
card.

**Scope**: add one section to `docs/ai/models/fr_regressor_v1.md`
citing Siniukov [238] and NEG-VMAF [235], plus a smoke eval on a
sharpened-distortion holdout under `ai/scripts/` to confirm whether
the regressor inherits the same vulnerability profile as upstream
VMAF.

**Success criterion**: model card updated, smoke eval committed,
PLCC/SROCC delta on the sharpened-set documented.

### Long — `T7-BVI-AOM-INGEST` (1 week, ADR needed)

**Title**: add BVI-AOM as a second training corpus for
`fr_regressor_v2`.

**Scope**: mirror `BVI-DVC` ingest in
`ai/src/vmaf_train/data/datasets.py`, add a manifest under
`ai/src/vmaf_train/data/manifests/`, set up LOSO splits, retrain
`fr_regressor_v2` on combined BVI-DVC + BVI-AOM + KoNViD-1k corpus,
compare PLCC/SROCC against v1 baseline.

**Success criterion**: v2 ONNX checkpoint registered with PLCC ≥ v1 +
0.01 on held-out test set; LOSO results table in research digest.

## Bottom line

The paper is a codec-side review, so most of its 258 refs are out of
scope for a quality-metric fork. The actionable signal lives in §5
(Test Protocols) and §5.3 (Quality Measures): BVI-AOM as new training
data, NEG-VMAF / Siniukov as a hardening axis for the FR regressor,
ST-VMAF / Zhang-2021 as direct prior art for the FR regressor's
design space, and DISTS as a clean LPIPS-companion extractor
candidate.
