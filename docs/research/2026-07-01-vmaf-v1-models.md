# VMAF v1 Models — Research Digest

**Date:** 2026-07-01
**Author:** VMAFx maintainers
**Upstream:** Netflix/vmaf `v3.2.0` (2026-06-20)
**ADR:** [ADR-1122](../adr/1122-vmaf-v1-model-port.md)
**Sources:** Netflix TechBlog "VMAF v1: Good Is Not Good Enough" (2026-06);
`Netflix/vmaf` `resource/doc/models_v1.md`; `model/vmaf_v1.0.16/*.json`;
`libvmaf/src/model.c` (built-in model table).

---

## Summary

Netflix shipped a new generation of VMAF models — **VMAF v1** — in libvmaf
`v3.2.0` (June 2026). v1 is **not** a new fusion architecture: it is still a
libsvm ν-SVR (`model_type: LIBSVMNUSVR`) over quality-aware features. What
changed is the **feature set** and the **model calibration** for viewing
conditions. The headline change is that **VIF is removed** and **CAMBI
(banding) + a chroma feature are promoted to core features**. Netflix also
recommends measuring at **10-bit precision** even for 8-bit SDR content, adds a
4K model with a **[0, 110]** score range, and provides high-frame-rate (`_hfr`)
variants.

This digest records the exact model set, the feature delta, and the concrete
implications for the fork (which already implements most of the v1 features on
GPU). It backs [ADR-1122](../adr/1122-vmaf-v1-model-port.md).

---

## The v1 model set (`model/vmaf_v1.0.16/`)

All models are libsvm ν-SVR JSONs, built into libvmaf (`model.c` exposes
`src_vmaf_v1_0_16_*_json`), selectable via `--model version=…` or
`--model path=…json`.

| Scenario | Display | Viewing distance | Model file | Score range |
| --- | --- | --- | --- | --- |
| Standard 1080p | 1080p | 3H | `vmaf_v1.0.16_3d0h.json` | [0, 100] |
| Phone | 1080p | 5H | `vmaf_v1.0.16_5d0h.json` | [0, 100] |
| **4K (default)** | 2160p | 1.5H | `vmaf_v1.0.16_1d5h_2160.json` | [0, 100] |
| 4K consumer TV | 2160p | 3H | `vmaf_v1.0.16_3d0h_2160.json` | **[0, 110]** |

Plus `_hfr` variants under `model/vmaf_v1.0.16_hfr/` (built-in as
`src_vmaf_v1_0_16_hfr_3d0h_json`, `…_hfr_3d0h_2160_json`) calibrated for
~50/60 fps: a **five-frame temporal motion window** (differencing over frames
i−2, i, i+2) with moving-average smoothing, correcting v0's under-prediction at
high frame rates.

## Feature delta (the core change)

The v1 standard model's `feature_names` (from `vmaf_v1.0.16_3d0h.json`):

```text
Cambi_feature_cambi_score
Speed_chroma_feature_speed_chroma_uv_score
VMAF_integer_feature_adm3_score
VMAF_integer_feature_motion3_score
```

| | v0 (`vmaf_v0.6.1`) | **v1 (`vmaf_v1.0.16`)** |
| --- | --- | --- |
| features | adm2, **vif_scale0..3**, motion2 | **cambi**, **speed_chroma_uv**, **adm3**, **motion3** |
| count | 6 | **4** |
| VIF | 4 scales (dominant cost) | **removed** |
| banding | separate CAMBI metric | **core feature** |
| chroma | — | **core feature** (`speed_chroma`) |

- **VIF removed.** Netflix found VIF is the most expensive feature and, after
  refining the others, dropped it without accuracy loss — so v1 is **faster**.
- **CAMBI in the fusion.** Banding (previously a standalone metric) is now a
  VMAF feature, addressing v0's blindness to banding.
- **Chroma feature added** (`speed_chroma_uv`) — v1 accounts for chroma
  distortion, which v0 ignored (luma-only).
- **ADM → `adm3`, Motion → `motion3`** (specific variants; motion3 is the
  wider temporal window used more aggressively by the `_hfr` variants).

## Key behavioral changes

1. **10-bit is the recommended precision.** *"VMAF v1 should ideally be applied
   at 10-bit precision for SDR"* (models_v1.md) — even 8-bit encodes should be
   measured at 10-bit (preprocess both inputs) so CAMBI can resolve banding.
2. **Scores can exceed 100.** The 4K@3H model uses **[0, 110]** to quantify the
   4K-over-1080p benefit at 3H. Any `min(score, 100)` assumption is now wrong.
3. **CAMBI encode-side params.** `--model …:cambi.enc_width=W:cambi.enc_height=H:
   cambi.enc_bitdepth=B` passes the *pre-upscale* encode geometry to the CAMBI
   feature; merged into the model's CAMBI options (no separate CAMBI instance).
   Falls back to input W/H/bitdepth when unset.
4. **Selection unchanged.** Same `--model version=…` / `path=…` mechanism as v0;
   v1 and v0 coexist (v0 preserved under `models_v0.md`).

## Performance & accuracy (Netflix's claims)

- v1 **matches or outperforms** v0 on most datasets; notable gains on WATERLOO
  IVC 4K, Netflix Screen-Size Crowdsourcing, and datasets with chroma/banding
  artifacts and phone viewing.
- **Faster** than v0 (VIF — the compute bottleneck — is gone).
- Future: a detailed technical paper and an **HDR v1** model are planned.

## Implications for the fork (VMAFx)

The fork already implements **almost every v1 feature on GPU/SIMD**:

| v1 feature | Fork status |
| --- | --- |
| `adm3` | ADM extractor present (CUDA/SYCL/HIP); adm scales incl. scale 3 |
| `motion3` | present (`integer_motion3` column already emitted) |
| `cambi` | present (`cambi_sycl`, `integer_cambi_*`); **SYCL parity currently FAILING** (`test_sycl_cambi_parity` exit 1 — see Phase-3 verify) |
| `speed_chroma` | present (`speed_chroma_*`); **SYCL FAILS on Arc A380** (`test_sycl_speed_chroma_parity` SIGABRT: fp64-not-supported) |
| VIF | present but **no longer on the v1 critical path** |

Net: v1 is a **medium port**, not a rewrite. The load-bearing work:

1. **Port + build-in the 6 v1 JSONs** (`/add-model`), matching Netflix's
   built-in table.
2. **Fix SYCL CAMBI + speed_chroma** — these become *core* features for every
   v1 score. `speed_chroma_sycl` currently needs fp64 (SIGABRT on Arc A380,
   ADR-0220 territory); `cambi_sycl` parity fails. These are **v1 blockers on
   SYCL**.
3. **Handle the [0, 110] range** — audit CLI/filters/`--precision` and any
   clamp-to-100. The FFmpeg `libvmaf*` filters and `vf_libvmaf` patches must not
   truncate.
4. **VIF is now optional for v1** — keep for v0 compatibility, but the v1 path
   skips it (potential GPU speedup: the fork's whole VIF kernel stack is off the
   v1 critical path).
5. **10-bit alignment.** v1's 10-bit recommendation directly validates the
   zero-copy P010 path (ADR-1121, PR #1081) — the fork is already positioned to
   measure v1 at the recommended precision, unlike 8-bit tooling (e.g. FFMetrics).
6. **Golden gate.** The 3 Netflix CPU golden pairs are v0.6.1 — **do not touch**
   (CLAUDE.md rule 1). v1 needs **new** golden references in *separate* tests.
7. **Upstream sync.** `Netflix/vmaf v3.2.0` is the port target
   (`/sync-upstream` / `/port-upstream-commit`) — large: new models + VIF
   removal from fusion + CAMBI/chroma integration + CAMBI enc-params.

## Open questions for the port

- Does `adm3`/`motion3` in the fork bit-match upstream v1's feature definitions,
  or did upstream change the integer kernels for v1? (Verify against v3.2.0.)
- CAMBI enc-param plumbing through the FFmpeg filter AVOptions (ffmpeg-patch
  impact under CLAUDE.md rule 14).
- HDR v1 timing (Netflix "planned") — defer or scaffold.
