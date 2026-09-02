<!-- markdownlint-disable MD013 MD060 -->
# Research-0046 — Bristol VI-Lab dataset feasibility for tiny-AI training and parity soak

| Field             | Value                                                                                  |
| ----------------- | -------------------------------------------------------------------------------------- |
| **Date**          | 2026-05-02                                                                             |
| **Status**        | Reconnaissance only; no downloads, no code change                                      |
| **Companion ADR** | [ADR-0241](../adr/0241-bristol-bvi-cc-ingest.md) (Status: Draft)                       |
| **Tags**          | ai, fr-regressor, corpus, license, parity-soak, bristol, bvi                           |

## Why now

`fr_regressor_v1` (T6-1a, commit `e421d70`) is trained on Netflix
Public only — 9 reference + 70 distorted, ~37 GB, gitignored at
`.workingdir2/netflix/`. The codec-aware `fr_regressor_v2` plan in
[ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) and
[ADR-0235](../adr/0235-codec-aware-fr-regressor.md) needs a wider
codec sweep with subjective labels. Separately, the cross-backend
parity gate (`/cross-backend-diff`) currently runs over only the 3
Netflix golden pairs at `places=4`; we have no real-corpus soak.

The Bristol Visual Information Lab (David Bull's group, "VI-Lab")
publishes a family of public BVI-* datasets. Several are MOS-labelled
and codec-diverse. The user has asked for a feasibility report before
any download — idle GPUs are available, but storage and licence
posture have to be cleared first.

## 1. Inventory

Sizes are estimates from paper specs (sequence count × resolution ×
duration × bit-depth × 1.5 chroma factor); ranges where the lab page
does not state a download size. Format column: `YUV` means raw
4:2:0 planar, `Y4M` means raw with header, `MP4` means encoded
streams. All Bristol downloads are gated by a registration form
(MS OneDrive or `data.bris.ac.uk`); confirmation typically lands
inside 2 days for the OneDrive route.

| Dataset      | Content type                                  | Refs / Distorted          | Estimated raw size | Format                          | MOS / DMOS | Lab landing                                                                       |
| ------------ | --------------------------------------------- | ------------------------- | ------------------ | ------------------------------- | ---------- | --------------------------------------------------------------------------------- |
| BVI-CC       | Codec sweep (HM, AV1, VTM) at UHD/HD          | 9 ref / 306 dist          | ~250–400 GB        | YUV 4:2:0, 60 fps, 5 s clips    | Yes (DMOS) | <https://fan-aaron-zhang.github.io/BVI-CC/>                                       |
| BVI-DVC      | Deep-codec training corpus, 270p–2160p        | 800 sequences             | ~700 GB – 1.2 TB   | YUV 4:2:0 (10-bit per AOM use)  | No         | <https://fan-aaron-zhang.github.io/BVI-DVC/>                                      |
| BVI-AOM      | Successor to BVI-DVC; 4K + downsampled tiers  | 956 (239 unique 4K src)   | 124 GB packed      | YUV 4:2:0 10-bit, lossless H264 | No         | <https://github.com/fan-aaron-zhang/bvi-aom>                                      |
| BVI-HD       | HEVC + HEVC-SYNTH compression                 | 32 ref / 384 dist         | ~80–120 GB         | YUV 4:2:0, HD                   | Yes (DMOS) | <https://research-information.bris.ac.uk/en/publications/bvi-hd-...>              |
| BVI-HFR      | High-frame-rate study, up to 120 Hz           | 22 sequences              | ~50–90 GB          | YUV 4:2:0, HD                   | Yes (MOS)  | <https://fan-aaron-zhang.github.io/BVI-HFR/>                                      |
| BVI-SR       | Spatial-resolution study up to UHD-1          | 24 sequences              | ~40–80 GB          | YUV 4:2:0, HD/UHD               | Yes (MOS)  | <https://data.bris.ac.uk/data/dataset/1gqlebyalf4ha25k228qxh5rqz>                 |
| BVI-VFI      | Frame-interpolation quality                   | 108 ref / 540 dist        | ~150–250 GB        | YUV 4:2:0, 540p–2160p, 30–120fps| Yes (DMOS) | <https://github.com/danier97/BVI-VFI-database>                                    |
| BVI-SynTex   | CGI synthetic textures                        | 186–196 sequences         | ~30–60 GB          | YUV 4:2:0                       | Partial    | <https://data.bris.ac.uk/data/dataset/320ua72sjkefj2axcjwz7u7yy9>                 |
| BVI-RLV      | Low-light + paired clean ground truth         | 40 scenes (~30k frames)   | ~100–200 GB        | YUV / image sequences           | No (paired)| <https://ieee-dataport.org/open-access/bvi-lowlight-...>                          |

Notes:

- **BVI-CC** total comes out at 9 src × 4 resolutions × ~5 s × 60 fps
  × ~10-bit 4:2:0 plus 306 distorted; the raw-source footprint alone
  is on the order of 80–150 GB and the distorted leg adds the rest.
- **BVI-AOM** is the only entry with an authoritative size figure
  (124 GB before zip) because the GitHub repo states it explicitly.
- BVI-DVC and BVI-AOM are *training corpora* and ship without
  subjective labels; they're useful for parity soak and self-supervised
  feature extraction, not for fr_regressor_v2's labelled-MOS leg.

## 2. Licence audit

Common posture across the BVI-* family: research / academic use,
registration form before download, redistribution of the **raw
sequences** is restricted, but derivative products (extracted
features, computed metrics, statistics) are generally permissible
with attribution. Per-dataset detail:

| Dataset      | Headline licence                               | Can extract features? | Can publish parquet of features in repo? | Form required? | Redistribution of raw clips |
| ------------ | ---------------------------------------------- | --------------------- | ---------------------------------------- | -------------- | --------------------------- |
| BVI-CC       | Paper text cites CC-BY for derived data        | Yes                   | Yes, with attribution                    | Yes            | No (academic redistribution restricted) |
| BVI-DVC      | Custom academic; README required               | Yes                   | Likely yes; verify README                | Yes            | No (training-only clause)   |
| BVI-AOM      | Custom academic; per-source clauses; some clips CC-BY-NC-ND 3.0 (CableLabs) | Yes (research only) | Mixed — CableLabs subset prohibits derivatives; rest yes | No (direct S3) | No |
| BVI-HD       | Bristol research licence (registration)        | Yes                   | Yes (derived metrics)                    | Yes            | No |
| BVI-HFR      | Bristol research licence (registration)        | Yes                   | Yes                                      | Yes            | No |
| BVI-SR       | Bristol research licence (registration)        | Yes                   | Yes                                      | Yes            | No |
| BVI-VFI      | IP retained by Bristol; registration           | Yes                   | Yes                                      | Yes            | No |
| BVI-SynTex   | data.bris (public, CGI-derived)                | Yes                   | Yes                                      | Yes            | Possibly — CGI source may permit; verify |
| BVI-RLV      | IEEE DataPort open-access; sign-in only        | Yes                   | Yes                                      | Account        | Per IEEE DataPort terms     |

Specific flags to watch:

1. **BVI-AOM CableLabs subset** — CC-BY-NC-ND 3.0 means we cannot
   ship derivative files for those sequences. The published paper
   marks them; ingest must propagate that flag through to any
   parquet manifest so per-clip filtering is possible at training
   time and at parquet-publish time.
2. **BVI-DVC "training only" clause** — the dataset is licensed for
   training video coding tools. Using it as input to a tiny-AI
   *quality regressor* sits at the edge of that wording. Defensible
   under "objective metric research" but worth one explicit user
   confirmation before we bake it into a published model card.
3. **No dataset has a clean `redistribute raw clips` clause.** The
   fork must never check Bristol clips into the repo (`.gitignore`
   already covers the Netflix corpus pattern; mirror it for BVI).

## 3. Use-case fit

| Use case                                | Best fit                  | Why                                                                                   |
| --------------------------------------- | ------------------------- | ------------------------------------------------------------------------------------- |
| `fr_regressor_v2` codec-aware MOS       | **BVI-CC** (primary), BVI-HD (secondary) | DMOS labels + explicit codec axis (HM / AV1 / VTM) — directly fills the gap noted in ADR-0235 |
| Cross-backend parity soak (no MOS)      | **BVI-AOM** or BVI-DVC    | Highest sequence count, widest resolution range; no subjective labels needed for an ULP-diff gate |
| New metric validation (correlation)     | BVI-HD, BVI-VFI, BVI-HFR  | DMOS-labelled with diverse distortion families                                        |
| Frame-rate / temporal-feature work      | BVI-HFR, BVI-VFI          | Only datasets in the family that vary fps                                             |
| Low-light / pre-processing experiments  | BVI-RLV                   | Out of current scope; archive for later                                               |

`fr_regressor_v2` cares about (a) codec one-hot, (b) reliable MOS
labels, (c) reasonable diversity. BVI-CC's 9×34 = 306 labelled
distorted sequences across HM/AV1/VTM at four resolutions hits
all three. BVI-HD adds 384 HEVC-distorted sequences with DMOS at
HD only — useful as a held-out single-codec validation slice.

## 4. Effort to extract one dataset (BVI-CC)

BVI-CC is the smallest *useful* MOS-labelled candidate and has the
codec axis the v2 regressor needs.

```text
# 1. Submit registration form (manual; ~2 day SLA)
#    https://fan-aaron-zhang.github.io/BVI-CC/

# 2. Stage download (MS OneDrive link arrives by email).
#    Pull into the gitignored corpus root:
mkdir -p .workingdir2/bristol/bvi-cc
rclone copy onedrive:BVI-CC .workingdir2/bristol/bvi-cc \
    --transfers 4 --progress

# 3. Most BVI clips ship as raw YUV with sidecar names that encode
#    geometry (e.g. *_3840x2160_60fps_10bit_420.yuv). No ffmpeg
#    transcode is required to feed libvmaf — vmaf consumes raw YUV
#    directly. If a clip is delivered as Y4M, strip the header:
ffmpeg -i src.y4m -f rawvideo -pix_fmt yuv420p10le src.yuv

# 4. Feature dump per (ref, dist) pair using the existing harness:
ai/scripts/konvid_to_full_features.py \
    --corpus-root .workingdir2/bristol/bvi-cc \
    --manifest    ai/src/vmaf_train/data/manifests/bvi-cc.json \
    --backend     cuda \
    --out         ai/data/features/bvi-cc.parquet

# 5. Parquet lands at ai/data/features/bvi-cc.parquet (gitignored);
#    manifest stays in-repo, features stay out-of-repo.
```

Sizing: at ~125 ms/frame end-to-end on a single mid-range GPU and
~300 frames/clip, the 306 distorted clips are ~2.5 GPU-hours wall
clock; doubled for both ref and dist features puts the soak at
~5–6 GPU-hours. Fits comfortably in an idle overnight slot.

Disk: assume ~250 GB on `.workingdir2/bristol/bvi-cc` (raw YUV
plus the encoded distorted streams the dataset already ships
pre-encoded). The dev box must have at least 400 GB free before
download; otherwise stage onto an external NVMe and bind-mount
the corpus root.

## 5. Risks

1. **Storage blow-out.** Pulling more than one BVI-* set in
   sequence puts the box at TB scale fast. BVI-DVC alone is in
   the 700 GB – 1.2 TB band. Mitigation: ingest BVI-CC first,
   ship the manifest + parquet, *then* decide whether to pull
   BVI-AOM next; never download two simultaneously.
2. **MOS-scale mismatch.** Bristol uses **DMOS** (difference MOS,
   higher = worse) for BVI-CC, BVI-HD, BVI-VFI; Netflix Public
   uses MOS (higher = better). Naïvely concatenating the two
   training sets without an inversion + re-scale step poisons
   the regressor. Mitigation: every parquet emits an explicit
   `mos_convention` column (`netflix_mos`, `bristol_dmos`,
   `bristol_mos_inverted`); the training loader normalises to a
   single 0–100 "higher is better" scale before fitting.
3. **Licence misread on BVI-AOM CableLabs subset.** Publishing a
   parquet derived from CC-BY-NC-ND 3.0 source is a "no-derivs"
   violation. Mitigation: the AOM ingest must consult a per-clip
   licence map and exclude restricted clips from any
   redistributable artifact.
4. **Pre-encoded streams drift across BVI-CC versions.** The
   distorted leg is a fixed encode of HM 16.18 / AV1 0.1.0 /
   VTM 4.01. If Bristol re-uploads with a newer encoder version,
   the parquet's bitrate / quality columns become inconsistent.
   Mitigation: pin the codec-version triplet inside the manifest
   and refuse to re-ingest a corpus root with mismatched names.
5. **Form-gated download blocks reproducibility.** Future
   contributors cannot reproduce the parquet without their own
   registration + ~2 day wait. Mitigation: ship the *manifest*
   in-repo (sequence list, expected SHA-256, MOS column) and let
   the parquet itself remain a personal-build artifact.
6. **Parity-soak cost on cross-backend diff.** Running
   `/cross-backend-diff` over 306 clips × 4 backends × full
   feature set is ~24 GPU-hours, not an interactive operation.
   Mitigation: a `--bvi-cc-soak` weekly CI run, not a per-PR gate.

## 6. Recommendation

**Ingest BVI-CC first, behind a dedicated PR**, sized for ~1
week of work:

1. Submit the registration form today (out-of-band, by the user).
2. While waiting, land the manifest scaffold and parquet schema
   change against an empty corpus root: `bvi-cc.json` manifest,
   `mos_convention` column added to the feature parquet, loader
   support for inverted-DMOS normalisation, ADR proposing the
   ingest, doc page under `docs/ai/training-data.md`.
3. When the OneDrive link arrives, fill the corpus root, run the
   feature dump, ship the parquet locally (gitignored), and run a
   confirmatory `/cross-backend-diff` over a 10-clip subset as a
   parity smoke test before any tiny-AI fitting.
4. **Do not** pull BVI-DVC or BVI-AOM in the same PR; that's a
   separate decision once we've measured fr_regressor_v2's lift
   from BVI-CC alone.

Companion ADR draft sits at `docs/adr/0241-bristol-bvi-cc-ingest.md`
(Status: Draft, number unassigned) — it formalises the same
recommendation and lists the alternatives we walked.

## References

- Frontiers: <https://www.frontiersin.org/journals/signal-processing/articles/10.3389/frsip.2022.874200/full>
- BVI-CC site: <https://fan-aaron-zhang.github.io/BVI-CC/>
- BVI-DVC site: <https://fan-aaron-zhang.github.io/BVI-DVC/>
- BVI-AOM repo: <https://github.com/fan-aaron-zhang/bvi-aom>
- BVI-HFR site: <https://fan-aaron-zhang.github.io/BVI-HFR/>
- BVI-VFI repo: <https://github.com/danier97/BVI-VFI-database>
- BVI-RLV preprint: <https://arxiv.org/abs/2407.03535>
- Lab person page (gateway): <https://research-information.bris.ac.uk/en/persons/david-r-bull/datasets/>
- Companion: [Research-0033 — Bristol VI-Lab NVC review (2026-04 preprint)](0033-bristol-nvc-review-2026.md)
- Prior art on the fork: ADR-0042 (tiny-AI docs), ADR-0235 (codec-aware fr_regressor_v2), ADR-0019 (tiny-AI Netflix training)
- Memory: `project_netflix_training_corpus_local.md` (existing 37 GB Netflix Public corpus root)
- Source: `req` (user direction, 2026-05-02 — Bristol VI-Lab feasibility for tiny-AI training and parity soak)
