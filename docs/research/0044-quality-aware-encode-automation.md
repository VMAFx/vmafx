<!-- markdownlint-disable MD060 -->
# Research-0044: Quality-aware encode automation (`vmaf-tune`) — option-space digest

- **Date**: 2026-05-02
- **Companion ADR**: [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
- **Status**: Snapshot at proposal time. Phase A implementation
  PR(s) supersede the operational details; this digest stays as
  the "why we picked these axes" reference.

## Question

The fork has every quality input a per-title, per-shot, codec-aware
encode optimiser would need (VMAF + 8 supporting metrics + tiny-AI
fusion regressor + saliency + shot detection + per-shot CRF predictor

- FFmpeg patches + codec-aware vocabulary). What's the smallest tool
that closes the loop — drives FFmpeg, captures bitrate + quality,
recommends parameters — without locking the design to one codec, one
search strategy, or one quality target?

## Prior art surveyed

| Tool | What it does | What we'd borrow | What we'd improve |
|---|---|---|---|
| **av1an** (Rust) | AV1-only chunked encoder with optional `--target-quality` (VMAF bisect over CRF per chunk). Uses scenedetect for chunking. | The bisect strategy is solid: "encode at midpoint CRF, score, halve interval, repeat 3–5 times". Concretely: target VMAF ± 0.5, ≤ 5 encodes per chunk. | Multi-codec; drop the AV1-only assumption; use our VMAF + ssimulacra2 + lpips ensemble; consume *our* shot detector instead of scenedetect-py |
| **ab-av1** (Rust) | Single-clip CRF bisect against a target VMAF. AV1-only. No per-shot. | Same bisect shape; simpler than av1an's chunking. | Multi-codec; per-title (predict starting CRF instead of always starting at midpoint) |
| **Netflix Per-Title** (paper, 2015) | Complexity-bucket sources, pick CRF per bucket. Shaping the bitrate-quality curve from offline data. | The "predict CRF from source features" model — that's our Phase C. | Use canonical-6 + codec one-hot + resolution + framerate as the source descriptor (we already extract these); skip the manual bucketing |
| **Netflix Dynamic Optimiser** (paper, 2018) | Per-shot Lagrangian λ-optimisation across the title; convex-hull pruning of (R, D) candidates. | The convex-hull formulation is the right shape for Phase E (Pareto ABR ladder). | Out of scope for Phase A–C; revisit at Phase E |
| **Bitmovin Per-Title** (closed) | Source-classification-based CRF + ladder generation. SaaS. | Confirms the per-title-CRF approach is production-real. | We're open-source, AI-driven, multi-metric. Different deployment shape. |
| **x265 Zone files** (`--zones`) | Per-frame-range qp/bitrate/aq override map. Native to x265. | Phase D output format for x265. | None — consume directly |
| **x264 `--qpfile`** | Per-frame qp override list. | Phase D output format for x264. | None — consume directly |
| **svt-av1 segment table** | AV1 zone-style overrides. | Phase D output format for svt-av1. | None — consume directly |
| **ffmpeg-bitrate-stats** (Python) | Parses ffmpeg encode logs for per-frame bitrate. | Reusable parser for the harness's bitrate-extraction step (could vendor or pip-depend). | Add metric extraction (we score *post*-encode via libvmaf, not from log lines) |

## Search-strategy axis

| Strategy | Encodes per target | When it wins | Notes |
|---|---|---|---|
| **Grid (full sweep)** | O(\|grid\|) | Producing the *training corpus* for Phase C; one-time per source | Only used in Phase A. Never used at inference. |
| **Coordinate descent** | 5–15 | Tuning a single quality knob (CRF) when other params are fixed | Simple, no ML deps. Phase A baseline strategy. |
| **Bisect (binary search)** | 4–6 | Single-knob target-quality (target VMAF ± 0.5) | Av1an-proven; this is Phase B's primary algorithm. |
| **Bayesian optimisation** | 8–20 | Multi-knob optimisation (CRF × preset × GOP × ref) | Adds `scikit-optimize` dep. Phase B+ optional. |
| **Per-title prediction + bisect refinement** | 1 (predict) + 2–3 (refine) | Phase C — fastest "given source, hit VMAF=X" | The whole point of training a predictor: skip 2–3 bisect rounds. |
| **Convex-hull / λ-sweep** | O(N²) candidates × M shots | Phase E ABR-ladder generation | Netflix Dynamic-Optimiser-style; deferred. |

**Decision**: Phase A ships **grid** (corpus generation) + **bisect**
(target-quality). Phase C swaps in the predictor as the "first
guess" for the bisect. Bayesian opt is opt-in, not the default.

## Codec-adapter interface

Every codec exposes a different parameter shape (CRF for x264/x265,
CQ for libvpx, --crf for svt-av1, --crf for libvvenc, neural codecs
have a λ-rate parameter). The harness must not branch on codec
identity in the search loop. Sketch:

```python
class CodecAdapter:
    name: str                       # "libx264", "libx265", "libsvtav1", ...
    quality_knob: str               # "crf", "cq", "lambda", ...
    quality_range: tuple[int, int]  # e.g. (0, 51) for x264
    quality_default: int            # mid-range default
    invert_quality: bool            # higher knob = lower quality? True for CRF/CQ

    def build_command(self, source, output, params: dict) -> list[str]: ...
    def parse_log(self, stderr: str) -> EncodeMetrics: ...   # bitrate, time, frames
    def two_pass_supported(self) -> bool: ...
    def emit_per_shot_overrides(self, shots: list[Shot]) -> str: ...  # qpfile / zone / segments
```

Phase A wires `libx264`. Each subsequent codec is a one-file
adapter that doesn't touch the harness or search loop.

## Codec scope (per popup `Q2` 2026-05-02)

| Codec | FFmpeg encoder | Phase A? | Quality knob | Phase D format | Notes |
|---|---|---|---|---|---|
| H.264 | `libx264` | **yes** | `-crf` | `--qpfile` | Highest leverage; 100× deployment of any other codec; corpus gen is fastest |
| H.265 / HEVC | `libx265` | Phase A+1 | `-crf` | `--zones` | Mature; second-most-deployed |
| AV1 | `libsvtav1` | Phase A+2 | `-crf` | segment table | Fastest AV1 encoder; corpus gen costs 5–10× x264 |
| VP9 | `libvpx-vp9` | Phase A+3 | `-crf` (CQ mode) | (no per-shot native) | YouTube-scale; fewer per-shot levers |
| H.266 / VVC | `libvvenc` | Phase A+4 | `-crf` | (TBD — VVenC has perceptual-QP overrides) | Newest standard codec; encoders still rapidly evolving |
| LCEVC (MPEG-5 Part 2) | `liblcevc-eilp` (3rd-party) | Phase A+5 | base-codec CRF + enhancement params | (enhancement-layer specific) | Two-codec system; harness needs to compose |
| EVC (MPEG-5 Part 1) | `libxeve` (3rd-party) | Phase A+5 | `-crf` | (TBD) | Royalty-friendly H.265 alternative |
| AVS3 | `libuavs3e` / `libxavs2` | Phase A+5 | `-crf` | (TBD) | Chinese standard; deployment is regional |
| **Neural codecs (DCVC family, NVC, CompressAI research models)** | (Python, PyTorch) | Phase A+6 (extras) | rate-λ | (no per-shot model) | Not via FFmpeg — research-grade Python encoders. Lives behind `pip install vmaf-tune[neural]` extra. The Bristol VI-Lab 2026 NVC review (`docs/research/0033-bristol-nvc-review-2026.md`) is the landscape map. |
| **JPEG-AI / image neural codecs** | (Python) | Out of scope | — | — | Image-only; not a video tool |

The phasing reflects "ship the codec adapter, gate on whether we
have an encoder corpus we can ourselves produce". `libx264` first;
neural codecs last because their corpus production is 100–1000× more
expensive per encode and the inference is too slow for live use cases.

## Training-corpus plan

Per-title CRF predictor (Phase C) and codec-aware FR regressor
(ADR-0235, currently BLOCKED) need (source, encode, score) tuples.
We can never redistribute third-party encodes; therefore we own
the encoder.

| Source | Sources we have | Encodes we own | Status |
|---|---|---|---|
| Netflix Public Dataset | 9 ref + 70 dis YUVs (37 GB, .workingdir2/netflix/) | We re-encode at Phase A grid | Sources: present locally per memory note 2026-04-27 |
| KoNViD-1k | sources + per-clip MOS | We re-encode at Phase A grid | Sources: CC BY 4.0, available |
| BVI-DVC (parts A+B+C+D) | sources + per-clip ratings | We re-encode at Phase A grid | Already used for vmaf_tiny_v2 |
| BVI-VC | sources | Optional Phase A+ corpus | TBD |

**Process**: Phase A's harness runs the grid sweep over every source ×
codec × CRF setting in our codec scope. Output is a parquet
schema:

```text
source_path | source_canonical_6 | resolution | framerate | duration_s
codec | preset | crf | extra_params_json
encode_path (gitignored) | encode_size_bytes | encode_time_s
vmaf | ssimulacra2 | lpips_sq | psnr_y | psnr_hvs | cambi
encoder_version | encoder_commit | ffmpeg_version
```

The encodes themselves stay gitignored under `.workingdir2/encodes/`
(or a configurable cache dir). Only the parquet ships — and only
after a licensing audit confirms the source provenance permits
publishing the *features + scores* (it does for all four corpora).

## What we deliberately *don't* solve in Phase A

1. **Live / latency-sensitive encoding.** This whole tool is
   batch-VOD-shaped. Live transcoding has totally different
   constraints (encode in real time, no second-pass). Out of scope.
2. **Audio.** `vmaf-tune` is video-only. Audio-quality automation
   is a separate problem with separate metrics (PESQ, POLQA, ViSQOL).
3. **Quality / bitrate constraints from CDN economics.** We
   produce the (bitrate, quality) curve; deciding the right point
   on it for a CDN budget is the operator's call, not ours.
4. **Subjective MOS prediction beyond what our metrics give us.**
   Our metrics are the truth surface; we're not building a new MOS
   model in this tool.
5. **Encoder selection.** The user picks the codec; we don't
   recommend "use AV1 instead of H.264". (A future Phase G could,
   given a corpus that compares codecs at iso-quality across
   bitrates, but that's a separate tool.)

## Risks the digest flags

- **Corpus-generation cost**: a 60-CRF-value × 4-codec × 100-source
  grid is 24 000 encodes. Some neural-codec encodes take minutes per
  clip. Phase A has to ship with a sampling story: stratify by
  resolution / motion class / codec; don't bulk-encode the full
  cross product.
- **Encoder-version drift**: pinning encoders is non-trivial because
  ffmpeg ships with whatever versions Ubuntu has. The harness
  records exact build IDs; CI uses a vendored / pinned set.
- **Per-shot CRF conflicts with rate control**: x264's
  `--qpfile` overrides VBV/CBR rate control. Phase D has to
  document the interaction and fall back to constant-CRF mode
  when the user wants per-shot.
- **Neural-codec adapters add a heavy dependency tree** (PyTorch,
  CUDA, model weights). They live behind an opt-in extra and
  can never be a hard dependency of `vmaf-tune` — Phase A+ users
  who want only x264 should not need PyTorch.
- **Scope creep**: every encode-tuning feature on the planet
  *could* be added here. The phasing exists to anchor "Phase A
  ships standalone, every later phase is gated on the prior
  corpus existing". Reviewers should reject Phase A→F bundling.

## Decision implications for the ADR

- Multi-codec from day one (codec-adapter interface — designed in
  Phase A, only `libx264` wired).
- Tools tree (`tools/vmaf-tune/`), C + Python hybrid.
- Phase A standalone (~1 week); A→F roadmap with hard gates.
- Per-title predictor (Phase C) gated on Phase A corpus.
- Per-shot (Phase D) gated on T6-3b landing.
- Neural-codec adapters gated on extras + their own corpus track.

## References

- av1an: <https://github.com/master-of-zen/Av1an>.
- ab-av1: <https://github.com/alexheretic/ab-av1>.
- Netflix Per-Title (2015): <https://netflixtechblog.com/per-title-encode-optimization-7e99442b62a2>.
- Netflix Dynamic Optimiser (2018):
  Katsavounidis 2018, "Dynamic optimizer — a perceptual video
  encoding optimization framework".
- Bitmovin Per-Title:
  <https://bitmovin.com/blog/per-title-encoding/>.
- ffmpeg `--qpfile`:
  <https://trac.ffmpeg.org/wiki/Encode/H.264#FAQ>.
- x265 zones:
  <https://x265.readthedocs.io/en/master/cli.html#cmdoption-zones>.
- svt-av1 segments:
  <https://gitlab.com/AOMediaCodec/SVT-AV1/-/blob/master/Docs/Parameters.md>.
- CompressAI research catalog:
  <https://github.com/InterDigitalInc/CompressAI>.
- Bristol VI-Lab 2026 NVC review:
  [`docs/research/0033-bristol-nvc-review-2026.md`](0033-bristol-nvc-review-2026.md).
- ADR-0235 (codec-aware FR regressor): codec one-hot vocabulary
  the per-title predictor will inherit.
- ADR-0223 (TransNet V2 shot detector): Phase D shot input.
- T6-3b backlog: per-shot CRF predictor, Phase D model.
