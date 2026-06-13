<!-- markdownlint-disable MD013 MD060 -->
# Research-0136: HDR/UGC Dataset License Audit (2026-05-15)

**Status:** Complete
**Date:** 2026-05-15
**Branch:** `research/hdr-ugc-dataset-license-audit-2026-05-15`
**Related ADR:** [ADR-0459](../adr/0459-vmaftune-panel-aware-recommendations.md) (panel-aware workstream)
**Companion task:** Batch 16 of the 2026-05-15 Gap-Fill Plan

---

## Summary

License, distribution-stability, and actionability assessment for the 13 HDR/UGC video quality
dataset leads enumerated in Audit Slice C.7 (2026-05-15). Findings directly inform which datasets
can be ingested into local training pipelines this week versus which require negotiation or
infrastructure investment before they become useful.

**Results:** 6 datasets are ACTIONABLE-NOW, 5 are BLOCKED (access or license restrictions),
1 is ASPIRATIONAL-scale (needs infrastructure), and 1 (CHUG) is already active. HDRSDR-VQA
introduced a new panel/display-aware workstream scoped in ADR-0459.

---

## Dataset Summary Table

| # | Dataset + URL | Scale | License | Distribution | HDR coverage | HFR coverage | Bit depth | Resolution | Subjective method | Actionability | Recommended next step |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | [Beyond8Bits](https://shreshthsaini.github.io/Beyond8Bits) | ~44k clips, ~1.5M crowd ratings | CC BY-SA 4.0 (metadata); NC restriction on video payload ("non-commercial research" + no redistribution + no commercial deployment) | Direct download via AWS S3; no sign-up wall | HDR-only — PQ/BT.2020, 10-bit HEVC ladder | 24/25/30/60 fps | 10-bit | 360p–1080p + source ref | Single-stimulus ACR, AMT workers, SUREAL-MLE aggregation | ACTIONABLE-NOW | Direct S3 download of metadata + selected clips; cite Chen et al. + comply with NC clause |
| 2 | [CHUG](https://shreshthsaini.github.io/CHUG/) | 856 source / 5,992 distorted videos, 211k ratings | CC BY-SA 4.0 | Direct download via AWS S3; no sign-up | UGC HDR (PQ 10-bit HEVC) | 120 fps clips confirmed (~189 clips); mixed 24/30/60/120 fps | 10-bit | Mixed 360p–1080p portrait + landscape | Single-stimulus ACR, AMT workers | **Already active** — local pipeline running | HFR normalisation patch; add `--metadata-jsonl` to live invocation |
| 3 | [YouTube SFV+HDR](https://media.withyoutube.com/sfv-hdr) | ~4k source contents, ~2k native HDR (per paper) | YouTube Terms of Service — research use only; no redistribution | JavaScript-rendered landing page; access appears to require YouTube research partner agreement | Mixed HDR + SDR | Unknown (YouTube typical: 24/30/60 fps) | 10-bit (HDR) | Up to 4K | Unknown — no public methodology doc found | BLOCKED-on-access | Contact YouTube Research team at media.withyoutube.com; partnership or signed agreement likely required |
| 4 | [HDRSDR-VQA](https://live.ece.utexas.edu/research/Bowen_SDRHDR/sdr-hdr-bowen.html) | 558 clips (31 open-sourced), 145 subjects; 22k pairwise JOD scores across 6 displays | Custom academic — "no royalty, no written agreement; use + distribute freely with citation" (Chen et al. 2025) | Google Form sign-up required; no NDA | Mixed HDR10 + SDR matched pairs | 50/60 fps clips documented | Not explicitly stated | Not explicitly stated | Paired comparison across 6 HDR TVs; JOD (Just-Objectionable-Difference) scores | ACTIONABLE-NOW | Complete Google Form; download open-sourced 31-video subset; begin panel-aware tuning design (ADR-0459) |
| 5 | [LIVE HDR Database](https://live.ece.utexas.edu/research/LIVEHDR/LIVEHDR_index.html) | 310 videos, 31 content sources, 66 subjects (40 for quality), 20k+ opinion scores | Custom academic — "no royalty, no written agreement; use + distribute freely with citation" (Shang et al. ICIP 2022) | Google Form sign-up; password protected | HDR10 only | 50/60 fps | Not stated | Not stated | Controlled-lab ACR; two ambient lighting conditions (dark lab + bright living room) | ACTIONABLE-NOW | Complete Google Form to receive password + link; download and ingest |
| 6 | [AVT-VQDB-UHD-2-HDR](https://github.com/Telecommunication-Telemedia-Assessment/AVT-VQDB-UHD-2-HDR) | 31 source videos (8K UHD-2 HDR) | CC BY-NC 4.0 (TU Ilmenau content) | Google Form sign-up for video sources; objective metrics and metadata on GitHub; checksums provided | HDR full — 8K UHD-2 | Not specified | Not specified (8K HDR → likely 10-bit) | 8K (UHD-2 = 7680×4320) | Not fully specified (paper in QoMEX 2024) | BLOCKED-on-access | Complete Google Form; note that 8K decode requires significant compute — assess infra fit before download |
| 7 | [IPI-MobileHDRVQA](https://zenodo.org/records/11544387) | 60 source videos, AV1 codec variants; 1.2 GB download (27.4 GB this version) | CC BY 4.0 | Direct Zenodo download; no sign-up required | UGC mobile HDR | Not specified | Not specified | Mixed mobile resolutions | ACR-HR (ACR with Hidden Reference); MOS + DMOS + 95% CI provided; lab study at Nantes University | ACTIONABLE-NOW | Direct Zenodo download — `curl https://zenodo.org/records/11544387/files/...` |
| 8 | [SHU-HDRQAD](https://github.com/SHU-HDRQAD/HDR-IQA-Dataset) | 1,409 distorted HDR images; 147 scenes; 6 distortion types | No license stated in repository | Google Drive download (no formal access control, but no explicit license grant) | HDR images (IQA not VQA) | N/A — image dataset | Not stated | Not stated | MOS values provided; methodology details absent from README | BLOCKED-on-license | Request explicit license from maintainers before ingesting; image-only limits utility for VQA training |
| 9 | [BrightVQ](https://github.com/shreshthsaini/BrightVQ/) | 300 source / 2,100 transcoded videos; 73,794 crowd ratings | CC BY-NC 4.0 | Direct AWS S3 download (browser or CLI); full-dataset link marked "COMING SOON" at time of audit | HDR UGC — 360p to 1080p | Not specified | Not specified | 360p–1080p | Single-stimulus ACR, AMT workers; MOS provided | BLOCKED-on-access | Monitor GitHub for full-dataset download link; contact author (shreshthsaini) for early access |
| 10 | [BVI-AOM / BVI-CC](https://github.com/fan-aaron-zhang/bvi-aom) | 956 sequences (239 unique 4K sources); 64 frames each; multi-resolution (480p–2176p) | Research-only; "material shall only be used for developing future video coding standards, for training or performance evaluation of test models in JVET and AOM" | Direct S3 download (124 GB); no NDA but restricted to JVET/AOM standard development | SDR only (BVI-AOM is SDR despite being 10-bit) | 24–50 fps | 10-bit, 4:2:0 | 480p–2176p (source 4K) | No subjective; objective only (PSNR-Y, VMAF) | BLOCKED-on-license | Do not ingest for general VQA training — license restricts to JVET/AOM use; use BVI-DVC (already ingested) instead |
| 11 | [SJTU HDR Video Sequences](https://medialab.sjtu.edu.cn/tag/dataset/) | 16 video sequences | CC BY-NC-ND 4.0 | Academic-only; contact admin for Dropbox/file-share access; no public direct download link | HDR — Sony RAW 16-bit, OpenEXR output, S-Gamut/S-Log3, BT.2020, SMPTE ST.2084 | Not specified | 16-bit raw (half-float 4:4:4 RGB OpenEXR) | UHD (4K) | No subjective ratings | BLOCKED-on-access | Contact SJTU Media Lab admin; note ND clause prohibits format conversion or derivative works |
| 12 | [HDR-VDC](https://github.com/gfxdisp/HDR-VDC) | 16 reference + 132 test videos; 30 subjects; JOD scores | CC BY 4.0 | DOI repository (Cambridge Apollo): `https://doi.org/10.17863/CAM.107964`; direct download | HDR (PQ, LG G2 OLED displays) | Not specified | Not specified | 720p, 1080p, 4K | Pairwise comparison; JOD (Just-Objectionable-Difference); 2 luminance levels × 2 viewing distances | ACTIONABLE-NOW | Direct Cambridge Apollo DOI download; CC BY 4.0 permits training use with attribution |
| 13 | [AGH/NTIA/Dolby (CDVL)](https://www.cdvl.org/) | Unknown — content search required after registration | No specific license quoted on public pages; "freely available for research and development" per site copy | Registration required ("free to join"); no NDA mentioned; members-only access | Possibly HDR (Dolby involvement suggests HDR); confirmation requires access | Unknown | Unknown | Unknown | Unknown | BLOCKED-on-access | Register at cdvl.org (free); search for `AGH-NTIA-Dolby` after login; confirm HDR coverage and license before committing to ingest |

---

## Per-Dataset Prose Assessment

### 1. Beyond8Bits

Beyond8Bits is the largest HDR-UGC corpus audited, with approximately 44,000 transcoded
videos derived from 6,861 crowd-sourced source videos and over 1.5 million AMT quality
ratings. The dataset is methodologically rigorous: ratings use SUREAL Maximum Likelihood
Estimation aggregation, workers are screened for HDR-display ownership, and a golden-set
calibration step filters low-quality annotators. All content is PQ/BT.2020 10-bit HEVC
across a five-rung bitrate ladder (0.2–5 Mbps) at 360p, 720p, and 1080p — matching the
CHUG transcoding philosophy closely.

The license situation requires care. Metadata is released under CC BY-SA 4.0, but the
video payloads carry an additional "non-commercial research" restriction that prohibits
redistribution outside the approved S3 mirror and deployment in commercial products without
a separate license from UT Austin / YouTube. This NC clause is compatible with internal
training but rules out shipping model weights that are demonstrably trained on this corpus
without legal review, mirroring the existing BVI-DVC posture (weights ship locally only).

Frame rate coverage — 24/25/30/60 fps — does not include 120 fps, making it less affected
by the HFR normalisation gap identified in CHUG's audit. It is the natural scale-up corpus
to run after CHUG extraction completes.

### 2. CHUG (already active)

Already actively ingesting locally (`chug_pipeline.pid` running, 3,597 clips complete at
audit time). Key open issues from Audit Slice H: missing `--metadata-jsonl` flag in the
live invocation (content-split columns absent from the finished parquet), and the HFR
normalisation gap (189 clips at 120 fps, ~525 at 60 fps, all processed without
`motion_fps_weight` normalisation). Neither issue is blocked on external access; both are
internal pipeline fixes tracked as open items. No further access work required.

### 3. YouTube SFV+HDR

The `media.withyoutube.com/sfv-hdr` landing page is JavaScript-rendered and returns only
a title heading via WebFetch, consistent with the page being a research partner portal
rather than a public download page. No paper preprint with a verified public arXiv ID was
located. Based on secondary references in the HDR VQA literature, the dataset covers
approximately 4,000 content items of which roughly 2,000 are native HDR; methodological
details are not publicly documented. Access likely requires a YouTube Research partnership
or signed data-sharing agreement. This dataset represents the highest potential scale in
the audit cohort but is the most opaque regarding access terms. A direct contact with the
YouTube Research team is the only viable path this week.

### 4. HDRSDR-VQA

This is the most strategically important dataset in the audit for the vmaf-tune toolchain
specifically. The dataset covers 960 total video clips (558 with public open-source content;
the remaining 145 subjects' VoD and live sports clips are unavailable due to copyright). The
distinguishing feature is the six-display pairwise evaluation design: 145 participants rated
HDR10 versus matched SDR versions on six distinct HDR televisions, producing scaled JOD
scores. This introduces display-type variability data that no other dataset in the cohort
provides, making it the unique resource for the panel-aware vmaf-tune training workstream
scoped in ADR-0459. The license is permissive academic (no NDA, no royalty, citation
required). Accessing the 31 open-sourced videos via Google Form is the immediate action.
The 10 VoD clips and 10 live sports clips remain unavailable due to third-party copyright.

### 5. LIVE HDR Database

The LIVE HDR database provides 310 video clips from 31 source contents, evaluated by 66
participants (40 for the quality scoring task) under two ambient conditions. Its license is
permissive academic (identical clause to HDRSDR-VQA: no royalty, no written agreement,
cite the paper). The Google Form access gate is a minor friction; no NDA or institutional
affiliation requirement is stated. The dataset is complementary to HDRSDR-VQA in that it
studies ambient viewing condition effects (dark lab versus bright living room) rather than
display panel variability. At 20,000+ opinion scores it is moderately sized and appropriate
for validation of panel-aware or ambient-condition-aware quality models.

### 6. AVT-VQDB-UHD-2-HDR

This is the only 8K HDR corpus in the audit. The CC BY-NC 4.0 license permits non-commercial
research use. The Google Form access gate implies active curation by TU Ilmenau. The
principal limitation for immediate ingestion is infrastructure: 8K HDR decode requires
significantly more compute than the 1080p CHUG clips, and the local NVIDIA GPU pipeline
may not efficiently process 8K without additional buffering. The QoMEX 2024 paper should
be reviewed before committing to download (124 GB S3 equivalent is the BVI-AOM reference
point; this dataset is likely similar scale). The subjective methodology is not fully
documented on the GitHub page; details are in the QoMEX paper. Not a week-1 priority but
represents the only available 8K HDR quality corpus.

### 7. IPI-MobileHDRVQA

The simplest acquisition path of the 13: a CC BY 4.0 Zenodo record with direct download,
no registration, 1.2 GB for the primary download (27.4 GB for this version, 197 GB total
across all versions). The AV1 codec coverage is a gap relative to CHUG (HEVC-encoded), but
AV1 represents an increasing fraction of real-world HDR delivery. The ACR-HR methodology
with hidden reference is slightly richer than standard ACR, enabling DMOS computation. At
60 source videos it is small but immediately actionable without infrastructure investment.
The mobile-oriented resolution mix (portrait mode content) adds a domain the CHUG pipeline
does not cover. Download this week; run standard VMAF feature extraction against the AV1
bitstreams.

### 8. SHU-HDRQAD

This is an image quality (IQA) dataset, not a video quality dataset. At 1,409 HDR images
across 147 scenes it has reasonable IQA scale, but the complete absence of an explicit
license in the GitHub repository is a blocker: the Google Drive hosting provides no formal
access controls but also provides no rights. Using unlicensed data for training model weights
that ship in a commercial fork is legally risky. Additionally, the VQA training pipeline
requires temporal content; a static image dataset has limited utility for training motion
or compression-artifact quality models. The recommended action is to contact the maintainers
requesting an explicit license grant — if they respond with CC BY or similar, the dataset
could serve as an HDR IQA validation set. Skip for the near term.

### 9. BrightVQ

BrightVQ is architecturally very similar to Beyond8Bits and CHUG (same UT Austin + AMT +
AWS S3 pipeline, CC BY-NC 4.0). At 73,794 ratings from 300 source videos it is substantially
smaller than Beyond8Bits. The critical finding is that the full-dataset download link was
marked "COMING SOON" at the time of audit (2026-05-15). This is not a license blocker — CC
BY-NC 4.0 permits research use — but it is a practical access blocker. The dataset is from
the same research group (Shreshth Saini, UT Austin) that published Beyond8Bits and CHUG,
suggesting the data will become available through the same S3 mechanism. Monitor the GitHub
repository; contact the author for early access if the timeline aligns with training needs.

### 10. BVI-AOM / BVI-CC

Despite the attractive technical specifications (10-bit, multi-resolution, 124 GB corpus,
VMAF evaluation baseline), BVI-AOM's license explicitly restricts use to "developing future
video coding standards" and "training or performance evaluation of test models in JVET and
Alliance for Open Media." This restriction is incompatible with general vmaf-tune model
training. The BVI-DVC corpus from the same Bristol VI Lab group is already ingested (PR #310,
ADR-0310) under a research license; BVI-AOM adds no HDR content (it is confirmed SDR 10-bit)
and carries a narrower license. Do not ingest BVI-AOM for the vmaf-tune pipeline.

### 11. SJTU HDR Video Sequences

SJTU provides 16 professional-grade HDR sequences captured with Sony F65/F55 cameras at
16-bit raw, stored as half-float 4:4:4 RGB OpenEXR after SMPTE ST.2084 tone mapping. The
technical quality is exceptional — over 14 stops of dynamic range, BT.2020 primaries,
S-Gamut/S-Log3. The access model (contact admin, download via Dropbox/file share) is
manageable. However, two issues make this a low priority: (1) the CC BY-NC-ND 4.0 license's
NoDerivatives clause explicitly prohibits format conversion, meaning the OpenEXR files cannot
legally be decoded to YUV for VMAF feature extraction without constituting a "derivative
work" under a strict reading; (2) there are no subjective quality ratings — the sequences
serve as pristine HDR reference content for codec evaluation, not as a VQA training corpus.
Contact SJTU to clarify the derivative-works scope before proceeding.

### 12. HDR-VDC

HDR-VDC (Cambridge gfxdisp lab, 2024) is a clean CC BY 4.0 corpus with 148 videos, 30
subjects, and pairwise JOD scores across AV1 compression and Lanczos upscaling distortions
at three resolutions (720p, 1080p, 4K) on a high-quality LG G2 OLED display. The CC BY 4.0
license is the most permissive in the audit cohort — it permits redistribution and derivative
works with attribution, making it suitable as a training corpus without the NC restrictions
on weight shipping that Beyond8Bits and BrightVQ carry. The DOI-linked Cambridge Apollo
repository provides a stable, archival download. The primary limitation is scale (30 subjects,
148 clips versus CHUG's 5,992). It is a strong validation set for the HDR-VDC quality
dimension specifically (compression + upscaling artefacts on OLED). Download immediately.

### 13. AGH/NTIA/Dolby (CDVL)

The Consumer Digital Video Library (CDVL.org) requires free registration to access content.
The public-facing pages state that content is "freely available for research and development
purposes" once registered, but no specific license text is available without logging in. The
AGH/NTIA/Dolby sequences are not listed on the public pages; their existence and HDR coverage
must be verified post-registration. Dolby's involvement is a positive signal for HDR content
but also introduces potential IP complexity. The CDVL is a well-established academic resource
(long history of MPEG and HEVC evaluation content) and registration is low-friction. Register
this week; search for the AGH/NTIA/Dolby collection; evaluate the license and HDR metadata
before committing further.

---

## Top 3 Priority Recommendations

### Priority 1 — IPI-MobileHDRVQA (Zenodo CC BY 4.0) — Download this week

**Why first:** Zero friction. No registration, no sign-up, no NDA. CC BY 4.0 permits training
use and derivative weight shipping without NC restrictions. At 60 source videos and 1.2–27 GB
it fits on local storage, and AV1 content complements CHUG's HEVC coverage. Feature extraction
can run using the existing CPU pipeline while the CHUG CUDA run continues.

**Concrete action (this week):** `curl -L https://zenodo.org/records/11544387/files/<manifest>`
then run `extract_k150k_features.py` (NR mode) or wait for the CHUG `chug_extract_features.py`
FR-pair adaptation to handle AV1 bitstreams. Open a BACKLOG entry: T-IPIHDRVQA-INGEST.

### Priority 2 — HDRSDR-VQA (Google Form, permissive academic) — Apply today

**Why second:** Unique data. No other dataset in the cohort provides per-display-type pairwise
HDR-vs-SDR quality scores. This data is the direct prerequisite for the panel-aware vmaf-tune
workstream (ADR-0459). The Google Form gate is minutes of friction. The 31 open-sourced clips
are available immediately after form submission; the 145-participant JOD score files likely
accompany them.

**Concrete action (this week):** Submit Google Form at
`https://live.ece.utexas.edu/research/Bowen_SDRHDR/sdr-hdr-bowen.html`; download the
open-source 31-clip subset; extract VMAF features and JOD scores; design the panel-aware
MOS-head schema (see ADR-0459 §Context).

### Priority 3 — HDR-VDC (Cambridge Apollo, CC BY 4.0) — Download this week

**Why third:** Most permissive license in the cohort (CC BY 4.0, no NC restriction). The
AV1 + Lanczos upscaling distortion types are complementary to CHUG's HEVC compression focus.
The Cambridge Apollo DOI link (`https://doi.org/10.17863/CAM.107964`) provides archival
stability. At 148 clips it is small but immediately usable as a validation holdout set or
supplementary training shard.

**Concrete action (this week):** `wget` or `curl` from the Cambridge Apollo download page;
add to BACKLOG as T-HDRVDC-INGEST. Extract VMAF features using existing CPU pipeline.

---

## New Workstream Surfaced — Panel/Display-Aware vmaf-tune

### Background

HDRSDR-VQA introduces a six-display pairwise evaluation design (OLED/QLED/LCD panels of
varying peak luminance and color volume) that reveals substantial display-type-dependent
perceptual quality differences for the same HDR10 + SDR video pair. This is not captured by
any existing vmaf-tune model or recommendation workflow: the current `vmaf-tune recommend`
path is display-agnostic — it assumes a generic high-end HDR display. In the field, HDR
content is consumed on a range of panels where the same CRF setting can be perceptually
optimal on one display and visibly degraded on another.

### Proposed T-number

`T-VMAFTUNE-PANEL-AWARE` — Panel/display-aware vmaf-tune recommendation workstream.

**Scope:** Train a display-conditioned MOS/JOD head that accepts a `display_type` or
`peak_luminance_nits` input feature alongside the standard VMAF feature vector. The head
produces display-conditioned quality estimates enabling `vmaf-tune recommend --display-profile
{oled|qled|lcd|generic}`. Data source: HDRSDR-VQA six-display JOD scores + HDRVDC OLED
measurements as a second signal.

**Estimated effort:** T4 (medium: data ingestion 1–2 days, model head training 1 day,
CLI integration 2 days, docs 1 day). Not blocked on any open PR.

### ADR Reference

See [ADR-0459](../adr/0459-vmaftune-panel-aware-recommendations.md) (Status: Proposed) for
the full decision scaffold, alternatives considered, and consequences.
