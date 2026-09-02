<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1122: Adopt and port VMAF v1 models (opt-in, v0.6.1 stays default)

- **Status**: Proposed
- **Date**: 2026-07-01
- **Deciders**: Lusoris Dev (<dsp@mvdnet.org>)
- **Tags**: `vmaf`, `models`, `upstream-port`, `cambi`, `chroma`, `sycl`, `golden-gate`

## Context

Netflix shipped **VMAF v1** in libvmaf `v3.2.0` (2026-06-20) — a new generation
of models (`model/vmaf_v1.0.16/`). It is still a libsvm ν-SVR fusion, but the
**feature set changed**: VIF is removed, and CAMBI (banding) plus a chroma
feature (`speed_chroma_uv`) become core features; ADM/motion move to the
`adm3`/`motion3` variants. v1 adds viewing-condition-calibrated models (`1080p@3H`,
`phone@5H`, `4K@1.5H`, `4K@3H`), an `_hfr` high-frame-rate variant, a **[0, 110]**
score range for 4K@3H, CAMBI encode-side parameters, and a recommendation to
measure at **10-bit** even for 8-bit SDR. Details and citations are in the
[research digest](../research/2026-07-01-vmaf-v1-models.md).

The fork tracks Netflix/vmaf and already implements almost all v1 features on
GPU/SIMD (`adm3`, `motion3`, CAMBI, `speed_chroma`). Two of them — `cambi_sycl`
and `speed_chroma_sycl` — currently **fail on SYCL** (CAMBI parity failure;
speed_chroma needs fp64 and SIGABRTs on Arc A380). Under v1 these are no longer
optional: they are evaluated for **every** score. The fork must decide how far
to go, and whether v1 replaces or coexists with v0.

## Decision

We will **adopt VMAF v1 as a supported, opt-in model generation, keeping
`vmaf_v0.6.1` as the default.** Concretely:

1. **Port and build in the six v1 JSONs** (`vmaf_v1.0.16_{3d0h,5d0h,1d5h_2160,
   3d0h_2160}` + the two `_hfr` variants) via `/add-model`, matching upstream's
   built-in table. Selectable via `--model version=vmaf_v1.0.16_3d0h` etc.
2. **Fix the SYCL core features v1 depends on** — `cambi_sycl` parity and
   `speed_chroma_sycl` (fp64-free per ADR-0220) — as a hard prerequisite, since
   both are core v1 features on every backend.
3. **Support the [0, 110] range** end-to-end (CLI, `--precision`, the FFmpeg
   `libvmaf*` filters and `vf_libvmaf` patches) — no clamp-to-100.
4. **Plumb the CAMBI encode-side params** (`cambi.enc_width/enc_height/
   enc_bitdepth`) through the model-option parser and the FFmpeg filter AVOptions.
5. **Add v1 correctness tests against new v1 reference values** in *separate*
   test files. The three Netflix v0.6.1 CPU golden pairs and their assertions
   remain **untouched** (CLAUDE.md rule 1).
6. **Keep VIF** for v0 compatibility; the v1 dispatch simply does not register
   it (off the v1 critical path).

The default model stays `vmaf_v0.6.1` for one release cycle to preserve the
golden gate and score-scale stability; v1 is opt-in until its GPU features and
tests are green across CPU/CUDA/SYCL/HIP.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Port v1 opt-in, v0 stays default (chosen)** | Tracks upstream; no golden-gate/score-scale disruption; ships when GPU features are green | Two model generations to maintain for a while | Selected — correctness-preserving, incremental |
| Adopt v1 as the new default (replace v0.6.1) | Single generation; latest accuracy by default | Breaks the v0 golden gate; changes the score scale ([0,110]); silently shifts every downstream number | Rejected — violates the golden-data contract and surprises users |
| Don't port v1 (stay on v0) | No work | Falls behind Netflix; misses banding/chroma/phone accuracy; the fork's raison d'être is tracking upstream | Rejected — regressive |
| Port the JSONs only, defer SYCL CAMBI/chroma fixes | Fast to land models | v1 on SYCL would be broken (CAMBI/chroma are core, currently failing) — a shipped-but-broken backend | Rejected — v1 must work on the fork's flagship SYCL backend |

## Consequences

- **Positive**: the fork gains Netflix's latest, more-accurate, faster metric
  (VIF removed), with banding/chroma awareness and viewing-condition models; the
  10-bit recommendation aligns with the zero-copy P010 work (ADR-1121, PR #1081).
- **Negative**: two model generations coexist; the SYCL CAMBI/`speed_chroma`
  fixes are now on the critical path (they were tolerated as failing because v0
  doesn't fuse them). The `[0,110]` range touches user-visible output surfaces.
- **Neutral / follow-ups**: a `/sync-upstream` (or targeted `/port-upstream-commit`)
  against `Netflix/vmaf v3.2.0` for the fusion/VIF-removal changes; new v1 golden
  references; CAMBI enc-param AVOptions imply an ffmpeg-patch update (CLAUDE.md
  rule 14); HDR v1 (Netflix "planned") is deferred/scaffolded.

## References

- req: user request (2026-07-01) — "исследуй новые модели v1 … заведи ADR на порт v1".
- Research digest: [docs/research/2026-07-01-vmaf-v1-models.md](../research/2026-07-01-vmaf-v1-models.md).
- Upstream: Netflix/vmaf `v3.2.0`; `resource/doc/models_v1.md`; `model/vmaf_v1.0.16/*.json`.
- Related: [ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md) (10-bit zero-copy),
  [ADR-0220](0220-sycl-fp64-fallback.md) (fp64-free SYCL kernels — blocks `speed_chroma_sycl`).
