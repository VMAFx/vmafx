<!-- markdownlint-disable MD060 -->
# ADR-0346: FR-features-from-NR-corpus adapter pattern

- **Status**: Accepted
- **Status update 2026-05-15**: implemented; decode-original-as-
  reference pattern active in K150K extraction pipeline; ADR-0346
  cited in `ai/scripts/extract_k150k_features.py`.
- **Date**: 2026-05-09
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, methodology, fork-local

## Context

The fork's predictor schema for `fr_regressor_v2_ensemble`
([ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md))
and `fr_regressor_v3` ([ADR-0323](0323-fr-regressor-v3-train-and-register.md))
is **full-reference (FR)**: every row of the training corpus carries
the canonical-6 features (`adm2`, `vif_scale0..3`, `motion2`) which are
defined as comparisons between a clean *reference* video and a
*distorted* re-encode. The Netflix Public Drop and BVI-DVC shards
satisfy this contract because both ship raw / studio-master YUVs as
the reference.

The KonViD-150k shard ([ADR-0325](0325-konvid-150k-corpus-ingestion.md))
does not. Each row in
`/home/kilian/dev/vmaf/.workingdir2/konvid-150k/konvid_150k.jsonl`
(148,543 rows on disk) is a YouTube-UGC distorted MP4 plus a
crowdworker MOS — there is **no clean reference** because the source
upload is the only available master. Three downstream agents stopped
this session blocked on this missing primitive: K150K feature
extraction (`abd7b0f75b5b48f62`), encoder_internal trainer integration
(`abda108c8263491da`), `fr_regressor_v3` training
(`abd6ed552ac8cae60`).

Without an adapter the 148k+ K150K corpus (and any future LSVQ /
YouTube-UGC ingest) cannot power FR predictor learning. The fork
needs a deliberate pattern — one that is honest about the
"reference" being a re-decoded version of the upload, not a true
master, and that produces the FR row schema downstream trainers
already speak.

## Decision

We will adopt the **decode-original-as-reference** adapter pattern
for converting NR rows into FR corpus rows.

For each NR input MP4:

1. ffprobe the source for geometry (width, height, pix_fmt,
   framerate, duration).
2. ffmpeg-decode the upload to raw YUV. This raw YUV becomes the
   *de-facto reference* for every downstream comparison derived from
   this source.
3. For each CRF in a configured `crf_sweep` (default
   `(18, 23, 28, 33, 38)` — 5 points spanning visually-lossless to
   heavy compression on libx264), re-encode the raw YUV at that CRF
   via the existing
   [`vmaftune.corpus`](../../tools/vmaf-tune/src/vmaftune/corpus.py)
   FR pipeline.
4. Emit one corpus row per (source, encoder, preset, CRF) cell. The
   row schema is the existing :data:`CORPUS_ROW_KEYS` shape; no
   schema bump is required.

The implementation lives in
`tools/vmaf-tune/src/vmaftune/fr_from_nr_adapter.py` as a thin
orchestrator over `vmaftune.corpus.iter_rows`. K150K-specific glue
ships as `ai/scripts/extract_k150k_features.sh` (a runbook, not a
new pipeline).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| (a) Skip NR corpora entirely | No adapter complexity; FR semantics stay clean | Drops 148k K150K rows and the entire UGC distribution coverage that ADR-0325 set up to capture | Loses the cinematic-vs-UGC alignment ADR-0325 paid for; blocks the three stopped agents |
| (b) Synthesise a reference via denoising / super-resolution | Could approach a "true" master | Adds a learned-component dependency to the corpus pipeline (FastDVDnet / Real-ESRGAN) whose own quality is itself a training subject; circular | Out of scope; the fork already defers a denoiser pre-stage to a separate ADR track |
| (c) Cross-corpus transfer learning (FR pretrain + NR-MOS-only fine-tune) | Methodologically clean; preserves NR labels as-is | Different ADR scope (model-architecture decision, not corpus decision); does not unblock canonical-6 feature extraction itself | Defer; this ADR is about producing FR feature rows, not about training architecture. (c) can layer on top of the rows this adapter emits |
| (d) Decode-original-as-reference | Pragmatic; matches LIVE-VQA / LIVE-VQC / KonViD-1k synthetic-distortion methodology; each NR row produces N FR samples (corpus-multiplier effect); reuses the existing `vmaftune.corpus` Phase A pipeline verbatim | The "reference" carries upload-side artifacts (YouTube transcode, the original capture chain), so the FR scores measure *delta-vs-already-distorted-source*, not delta-vs-pristine-master — must be documented prominently | **CHOSEN** — only option that unblocks the three stopped agents this session, fits the existing corpus row schema, and has well-trodden academic precedent |

## Consequences

### Positive

- Unblocks the three stopped agents (K150K extraction, encoder_internal
  trainer integration, `fr_regressor_v3` training).
- Each NR input row produces `len(crf_sweep)` FR corpus rows — a
  ~5x corpus-size multiplier on K150K (148k → ~742k rows at default
  sweep size). This is also true for any future NR shard (LSVQ,
  YouTube-UGC, etc.).
- Reuses the existing `vmaftune.corpus.iter_rows` Phase A pipeline
  verbatim — no schema bump, no new row keys, no downstream
  trainer changes.
- Establishes the pattern for future NR ingests; the next NR shard
  ships as data + a runbook, not a new pipeline.

### Negative

- The "reference" is the re-decoded upload, not a pristine master.
  Any artifact in the upload chain (YouTube VP9 banding, capture
  noise, prior-encode blockiness) propagates into the "reference"
  and therefore into the FR features. Downstream consumers must
  treat NR-derived rows as *delta-vs-already-distorted* signal,
  not delta-vs-pristine signal. This caveat must be documented
  in `docs/ai/fr-from-nr-adapter.md` and in every JSONL output's
  comment header.
- Disk impact: 148k K150K uploads × ~5 MB/clip raw-YUV intermediate
  × ~5 CRFs/sweep is large. The adapter therefore decodes-once
  per source, sweeps all CRFs against that single intermediate,
  and cleans the intermediate when the sweep completes. Operator
  must size scratch accordingly (~750 GB peak for the full K150K
  pass at 720p uploads).
- Adds a re-encode wall-clock cost: the source MP4 was already
  encoded once by YouTube; we encode it again N times. K150K full
  pass is overnight-class on a single workstation.

### Neutral / follow-ups

- The `mos` column (KonViD MOS) flows through this adapter
  unchanged from the K150K JSONL; downstream MOS-head trainers
  (sibling `mos_regressor_v1` per ADR-0325) consume it as-is.
- The `vmaftune.corpus.iter_rows` pipeline does not need changes.
  The adapter is purely additive.
- Production-flip gate semantics
  ([ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md)) are
  unchanged. NR-derived rows count toward the same held-out PLCC
  / SROCC gate; they do not silently widen tolerances.

## References

- LIVE-VQA: Seshadrinathan, Soundararajan, Bovik, Cormack —
  *Study of subjective and objective quality assessment of video*,
  IEEE TIP 2010 — synthetic-distortion VQA methodology where each
  pristine reference is encoded at multiple CRFs / bitrates to
  generate the distortion set.
  <https://live.ece.utexas.edu/research/quality/live_video.html>
  (accessed 2026-05-09)
- LIVE-VQC: Sinno, Bovik — *Large-Scale Study of Perceptual Video
  Quality*, IEEE TIP 2019 — UGC-domain companion to LIVE-VQA.
  <https://live.ece.utexas.edu/research/LIVEVQC/LIVEVQC.html>
  (accessed 2026-05-09)
- KonViD-1k: Hosu, Hahn, Jenadeleh, Lin, Men, Szirányi, Li, Saupe —
  *The Konstanz Natural Video Quality Database (KonViD-1k)*,
  QoMEX 2017 — UGC dataset with crowdworker MOS labels (the K150k
  predecessor we adopt at scale via ADR-0325).
  <https://database.mmsp-kn.de/konvid-1k-database.html>
  (accessed 2026-05-09)
- [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  — `fr_regressor_v2_ensemble` real-corpus retrain (FR consumer of
  these rows).
- [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) — BVI-DVC ingestion
  (sister FR shard; native FR, no adapter needed).
- [ADR-0323](0323-fr-regressor-v3-train-and-register.md) —
  `fr_regressor_v3` training (FR consumer of these rows).
- [ADR-0325](0325-konvid-150k-corpus-ingestion.md) — KonViD-150k
  corpus ingestion (the NR shard that motivates this adapter).
- Stop-reports from the three blocked agents:
  `abd7b0f75b5b48f62` (K150K feature extraction),
  `abda108c8263491da` (encoder_internal trainer integration),
  `abd6ed552ac8cae60` (`fr_regressor_v3` training).
- Source: `req` (direct user direction, this session) — the user
  identified the missing FR-from-NR primitive after three agents
  stopped on it consecutively and requested a deliberate ADR + a
  thin adapter implementation, citing LIVE-VQA precedent.
