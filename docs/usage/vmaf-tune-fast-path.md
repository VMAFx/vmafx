<!-- markdownlint-disable MD060 -->
# `vmaf-tune fast`

`vmaf-tune fast` returns a recommended CRF for a target VMAF in seconds to
minutes instead of running a full grid. It samples candidate CRFs with
Optuna's TPE sampler, scores each trial with the `fr_regressor_v2` proxy on
canonical-6 probe features, and then runs one real encode plus libvmaf verify
pass at the chosen CRF. It is opt-in: the slow `corpus` + `recommend` path
remains the ground truth ([ADR-0276](../adr/0276-vmaf-tune-fast-path.md),
[Research-0060](../research/0060-vmaf-tune-fast-path.md)).

The pick rule is the one every `vmaf-tune` command uses: among the CRFs whose
predicted VMAF meets the target, the one with the lowest predicted bitrate. A
CRF that misses the target ranks behind all of those, and when none meets it
the closest miss is returned. The Go `fast` applies the same objective.

`fast` reports `proxy_verify_gap`. When the gap exceeds `--proxy-tolerance` the
CLI still prints the recommendation but exits with code `3`, so a caller can
fall back to the slow grid.

## Install

The fast path needs Optuna, which ships as the `[fast]` extra. The core
package stays free of extra dependencies so corpus generation works on hosts
that never run `fast`.

```shell
pip install 'vmaf-tune[fast]'
# or, from a checkout:
pip install -e 'tools/vmaf-tune[fast]'
```

Production mode also needs `onnxruntime` for the proxy model, an `ffmpeg`, and
a `vmaf` binary for the verify pass.

## Quick start

Smoke mode swaps the proxy and verify pipeline for a deterministic synthetic
CRF-to-VMAF curve. It needs no ffmpeg, no ONNX Runtime and no GPU, so CI on a
bare host can still exercise the search loop:

```shell
vmaf-tune fast --smoke --target-vmaf 92 --n-trials 12
```

```json
{
  "encoder": "libx264",
  "n_trials": 12,
  "notes": "smoke mode — synthetic predictor; no ffmpeg / ONNX / GPU. See ADR-0276 + ADR-0304 + Research-0076 for the production path.",
  "predicted_kbps": 1954.27,
  "predicted_vmaf": 82.65,
  "proxy_verify_gap": null,
  "recommended_crf": 27,
  "smoke": true,
  "target_vmaf": 92.0,
  "verify_vmaf": null
}
```

A production run on a real source:

```shell
vmaf-tune fast \
    --src ref.yuv --width 1920 --height 1080 \
    --framerate 24 --pix-fmt yuv420p \
    --encoder libx264 --preset medium \
    --target-vmaf 92 \
    --crf-min 18 --crf-max 40 \
    --n-trials 30 \
    --score-backend auto \
    --output recommendation.json
```

## Flags

`--target-vmaf` is always required. Outside `--smoke`, `--src` is required and
`--width` / `--height` must be positive.

| Flag | Default | Meaning |
|---|---|---|
| `--src PATH` | none | Source video. Required outside `--smoke`. |
| `--width`, `--height` | `0` | Source geometry. Required outside `--smoke`. |
| `--pix-fmt` | `yuv420p` | ffmpeg pixel format for the probe and verify encodes. |
| `--framerate` | `24.0` | Reference framerate. |
| `--target-vmaf T` | none (required) | Quality target on the `[0, 100]` VMAF scale. |
| `--encoder` | `libx264` | Any registered codec adapter. |
| `--preset` | `medium` | Encoder preset for the probe and verify encodes. |
| `--crf-min`, `--crf-max` | `10`, `51` | TPE search range over the integer CRF axis. |
| `--n-trials` | `30` (production), `50` (smoke) | TPE trial budget. |
| `--time-budget-s` | `300` | Soft wall-clock cap for the Optuna loop, see [Time budget](#time-budget). |
| `--proxy-tolerance` | `1.5` | Largest proxy/verify gap in VMAF points before exit code `3`. |
| `--sample-chunk-seconds` | `5.0` | Probe-slice duration per TPE trial. |
| `--smoke` | off | Synthetic curve; no ffmpeg, ONNX or GPU. |
| `--score-backend` | `auto` | Verify-pass backend: `auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal`. See [score backends](vmaf-tune-score-backend.md). |
| `--ffmpeg-bin`, `--vmaf-bin` | `ffmpeg`, `vmaf` | Tool paths. |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | libvmaf model for the verify pass. |
| `--encode-dir` | `.workingdir/cache/vmafx-tune/fast` | Scratch directory for probe and verify encodes. |
| `--output` | stdout | JSON destination for the recommendation. |

Production mode accepts any registered codec adapter. The proxy's codec
one-hot has twelve slots (`ENCODER_VOCAB_V2`, ADR-0291): `libx264`, `libx265`,
`libsvtav1`, `libvvenc`, `libvpx-vp9`, the three NVENC and the three QSV
encoders, and `unknown`. Any other adapter (`libaom-av1`, AMF, VideoToolbox)
takes the `unknown` slot. The run says so on stderr and adds
`"proxy_encoder_slot": "unknown"` to the JSON; the verify encode still measures
the pick with the requested encoder. Smoke mode stays synthetic and
x264-shaped by design.

## Time budget

`--time-budget-s` is a real Optuna timeout. The search stops scheduling new TPE
trials once it expires, and any in-flight trial is allowed to finish so that
probe encodes are not cut off halfway. The `n_trials` field of the result
counts completed trials, so it can be lower than `--n-trials` when the budget
is hit.

## Output

The JSON payload has the same recommendation core as `vmaf-tune recommend`,
plus the fast-path diagnostics `verify_vmaf` and `proxy_verify_gap`, and
`proxy_encoder_slot` when the encoder took the proxy's `unknown` slot:

```json
{
  "encoder": "libx264",
  "target_vmaf": 92.0,
  "recommended_crf": 22,
  "predicted_vmaf": 92.41,
  "predicted_kbps": 4820.0,
  "n_trials": 30,
  "smoke": false,
  "notes": "production: TPE over 30 trials with v2 proxy; GPU verify gap = 0.612 VMAF (tolerance 1.50).",
  "verify_vmaf": 91.8,
  "proxy_verify_gap": 0.612,
  "score_backend": "cuda"
}
```

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Recommendation produced and the proxy/verify gap is within tolerance. |
| `2` | Argument or runtime setup error (missing `--src`, bad CRF range, unavailable score backend, failed probe). |
| `3` | The proxy/verify gap exceeded `--proxy-tolerance`. The recommendation is still emitted; fall back to the slow grid. |

## Fall back to the slow grid

The `||` chain below covers both the setup error (`2`) and the
out-of-distribution
case (`3`), so the slow grid is the safety net whenever `fast` is not
confident:

```shell
vmaf-tune fast --src ref.yuv --width 1920 --height 1080 \
    --target-vmaf 92 --output rec.json \
  || vmaf-tune recommend --source ref.yuv --width 1920 --height 1080 \
        --preset medium --target-vmaf 92 --output rec.json
```

If you already have a corpus, `vmaf-tune recommend --from-corpus corpus.jsonl
--target-vmaf 92` is the cheaper fallback; see
[`vmaf-tune-recommend.md`](vmaf-tune-recommend.md).

## What `fast` is not

`fast` is a recommendation shortcut, not a corpus generator. It still needs a
representative source clip, a usable encoder and a `vmaf` binary for the verify
pass. It is a sibling of [`vmaf-tune auto`](vmaf-tune-auto.md), not a child:
`auto` never dispatches `fast`. Per-shot parallelisation is a separate
integration with TransNet V2 and `vmaf-perShot`, see
[`vmaf-tune-per-shot.md`](vmaf-tune-per-shot.md).

## Probe extraction and normalisation

Each TPE trial runs a lightweight probe encode and feature extraction pass:

1. **Probe decode.** Probe encodes are container bitstreams (for example
   `.mp4`). Before extraction, `maybe_decode_distorted` decodes the container
   to a temporary raw YUV file clamped to `--sample-chunk-seconds`, and
   removes it afterwards.
2. **Canonical-6 features.** libvmaf extracts `adm2`, `vif_scale0` to
   `vif_scale3` and `motion2`. The pooled metric keys are `integer_adm2`,
   `integer_vif_scale0..3` and `integer_motion2`, with a fallback to
   per-frame averages.
3. **Normalisation.** Raw feature averages are standardised as
   `(x - mean) / std`, using `feature_mean` and `feature_std` from
   `model/tiny/fr_regressor_v2.json`. They then enter the ONNX proxy together
   with the 14-dimensional codec block (12-way `ENCODER_VOCAB_V2` one-hot
   plus normalised preset and CRF).
4. **Strict errors.** A probe encode or extraction that exits non-zero raises
   `RuntimeError` at once. Probe failures are never masked with zero-filled
   features (`[0.0] * 6`), so a corrupted trial cannot steer the search.

### Go twin parity

The Python extraction and normalisation contract matches the Go twin in
`pkg/fast` (`vmafx-tune fast`, see [`vmafx-tune-go.md`](vmafx-tune-go.md)).
`tools/vmaf-tune/tests/test_fast_parity.py` runs both on the same clip and
asserts identical raw pooled means and normalised features within 1e-6.

## Speedup model

Estimated speedups over the Phase A grid, from Research-0060 (upper bounds,
not measurements):

| Combination | Speedup vs the Phase A grid |
|---|---|
| Phase A grid (baseline) | 1x |
| `fast` (proxy + Bayesian + GPU verify) | about 20x to 50x |
| `fast` + NVENC (follow-up lever) | about 100x to 500x |

The production claim is gated on a recommendation-quality benchmark against the
slow grid.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the tool overview.
- [`vmaf-tune-fast-nr.md`](vmaf-tune-fast-nr.md) — the unrelated `--fast-nr`
  bisect speed-up.
- [`vmaf-tune-prefilter.md`](vmaf-tune-prefilter.md) — reuses the same TPE
  search engine.
- [`vmaf-tune-recommend.md`](vmaf-tune-recommend.md) — the slow-grid path.
- [`docs/ai/models/fr_regressor_v2.md`](../ai/models/fr_regressor_v2.md) — proxy
  model card.
- [ADR-0276](../adr/0276-vmaf-tune-fast-path.md) and
  [ADR-0304](../adr/0304-vmaf-tune-fast-path-prod-wiring.md).
