# `vmaf-tune` HDR knobs and clip-sampling

Two orthogonal `vmaf-tune corpus` flags shape the *what* and *how
much* of each scoring run:

- **HDR detection / forcing** ([ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md))
  — auto-injects PQ / HLG codec arguments and the HDR-VMAF model
  when the source has PQ / HLG signaling, with manual overrides.
- **Clip sampling** ([ADR-0301](../adr/0301-vmaf-tune-sample-clip.md),
  [ADR-0297](../adr/0297-vmaf-tune-encode-multi-codec.md))
  — score only the centre N-second slice of each source, trading
  a 1–2 VMAF-point fidelity delta for a near-linear wall-time win.

The base tool is documented in [`vmaf-tune.md`](vmaf-tune.md).

## HDR mode

`--auto-hdr` is the default. It runs `ffprobe` (or a custom binary
via `--ffprobe-bin`) on each source; on detection of PQ
(SMPTE-2084) or HLG (ARIB STD-B67) signaling, the corpus runner:

1. Injects the appropriate `colorspace` / `color_primaries` /
   `color_trc` arguments into the encode invocation, so the encoded
   stream re-tags as HDR.
2. Selects the HDR-VMAF model for scoring instead of the SDR
   default. (Note: HDR-VMAF *scoring* is currently deferred —
   ADR-0300 status is "Accepted (encode-side); HDR-VMAF scoring
   deferred (no shipped HDR model in tree)". Until the HDR model
   ships, the score axis falls back to the SDR model with an INFO-
   level warning.)

Mutually exclusive overrides:

| Flag              | Effect                                                                   |
|-------------------|--------------------------------------------------------------------------|
| `--auto-hdr`      | (default) detect via ffprobe; inject HDR flags only when signaled.       |
| `--force-sdr`     | Treat all sources as SDR; skip HDR detection and any HDR flag injection. |
| `--force-hdr-pq`  | Treat all sources as HDR PQ (SMPTE-2084) regardless of probe.            |
| `--force-hdr-hlg` | Treat all sources as HDR HLG (ARIB STD-B67) regardless of probe.         |

Force-mode is useful when the source is HDR-coded but has lost
signaling metadata (e.g. raw YUV with no container), or when you
want to deliberately mis-score an SDR source through the HDR
pipeline as a comparison baseline.

### Example — auto-detect + sweep one HDR source

```shell
vmaf-tune corpus \
    --source hdr_pq.mp4 \
    --width 3840 --height 2160 --pix-fmt yuv420p10le \
    --framerate 24 \
    --encoder hevc_nvenc --preset p4 --crf 22 --crf 26 \
    --out hdr_corpus.jsonl
# (auto-hdr; hevc_nvenc adapter receives PQ flags automatically)
```

## Clip sampling — `--sample-clip-seconds`

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 60 \
    --encoder libx264 --preset medium --crf 22 --crf 28 --crf 34 \
    --sample-clip-seconds 10 \
    --out corpus.jsonl
```

| Value         | Meaning                                           |
|---------------|---------------------------------------------------|
| `0` (default) | Score the full source. Most accurate, slowest.    |
| `N > 0`       | Score the centre `N`-second slice of each source. |

### When to sample

- **Fast iteration** during corpus-methodology development. A 60 s
  source sampled at 10 s gives a ~6× wall-clock speedup with a 1–2
  VMAF-point fidelity delta on diverse content (typical-case figure
  per ADR-0301 — content-type sensitivity is real; check on your own
  corpus before treating the figure as a contract).
- **Pre-flight smoke test** before committing to a multi-hour sweep.

### When not to sample

- **Production verdicts**. Any final ladder / per-title CRF /
  promote-vs-hold decision should run on the full source.
- **Short sources** where the centre slice would be most of the
  source anyway — sampling 10 s of a 12 s source nets you nothing
  but a ragged measurement.
- **Very-high-motion content** where the centre slice is not
  representative of the temporal distribution. Per-shot scoring (see
  [`vmaf-perShot.md`](vmaf-perShot.md)) is the correct tool there,
  not centre-clip sampling.

### Slice geometry

The slice is *centred*: a 60 s source with `--sample-clip-seconds 10`
extracts seconds 25.0 → 35.0. This matches Netflix's per-shot
sampler convention. If the requested slice is longer than the
source, the full source is used and an INFO-level warning is
emitted.

## Combined example

```shell
vmaf-tune corpus \
    --source hdr_test.mp4 \
    --width 3840 --height 2160 --pix-fmt yuv420p10le --framerate 24 \
    --encoder hevc_nvenc --encoder av1_nvenc \
    --preset p4 --crf 22 --crf 28 \
    --score-backend cuda \
    --sample-clip-seconds 10 \
    --auto-hdr \
    --out smoke.jsonl
```

A six-cell HDR corpus across two NVENC encoders, scored on CUDA,
with a 10 s centre-clip — a typical pre-flight smoke run before
the full 60 s sweep.

HDR flag dispatch is centralised in `vmaftune.hdr.hdr_codec_args()`.
The current table covers the HDR-capable `vmaf-tune` adapters:
`libx265`, `libsvtav1`, `libaom-av1`, `hevc_nvenc`, `av1_nvenc`,
`hevc_qsv`, `av1_qsv`, `hevc_amf`, `av1_amf`, `hevc_videotoolbox`,
and `libvvenc` plus container-level color tags for `libx264`.
HEVC hardware rows force `p010le` + `main10`; AV1 hardware rows force
`p010le`; software AV1 and VVenC rows carry global `-color_*` tags.
The HDR VMAF model is still a separate model-slot issue: until a
licensed `model/vmaf_hdr_*.json` exists, scoring falls back to the SDR
model with the documented warning.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the base tool, corpus + recommend
  flow.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md) —
  adapter matrix that the HDR mode injects encoder flags into.
- [`vmaf-tune-score-backend.md`](vmaf-tune-score-backend.md) — the
  orthogonal `--score-backend` flag.
- [ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md) /
  [ADR-0301](../adr/0301-vmaf-tune-sample-clip.md) — design
  decisions.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md)
  — audit that triggered this page.
