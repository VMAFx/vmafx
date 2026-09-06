# `vmaf-tune fast`

`vmaf-tune fast` is the opt-in recommendation shortcut for operators
who want a CRF answer without running a full Phase-A grid. It samples
candidate CRFs with Optuna's TPE sampler, scores each trial through the
`fr_regressor_v2` proxy on canonical-6 probe features, then runs one
real encode + libvmaf verify pass at the selected CRF.

The slow `corpus` + `recommend` path remains the ground truth. `fast`
reports `proxy_verify_gap`; when the gap exceeds `--proxy-tolerance`
the CLI exits `3` so callers can fall back to the slow grid.

## Quick Start

```shell
vmaf-tune fast \
    --src ref.yuv --width 1920 --height 1080 \
    --framerate 24 --pix-fmt yuv420p \
    --encoder libx264 --preset medium \
    --target-vmaf 92 \
    --score-backend auto \
    --output recommendation.json
```

Smoke mode keeps CI and local plumbing checks dependency-light:

```shell
vmaf-tune fast --smoke --target-vmaf 92 --n-trials 12
```

## Time Budget

`--time-budget-s` is a real Optuna timeout. The search stops scheduling
new TPE trials after the timeout expires; any in-flight trial is allowed
to finish so probe encodes are not interrupted halfway through. The
`n_trials` field in the JSON result records completed trials, so it may
be lower than `--n-trials` when the time budget is hit.

## Output

The JSON payload carries the same recommendation core as
`vmaf-tune recommend`, plus fast-path diagnostics:

```json
{
  "encoder": "libx264",
  "target_vmaf": 92.0,
  "recommended_crf": 22,
  "predicted_vmaf": 92.41,
  "predicted_kbps": 4820.0,
  "n_trials": 30,
  "smoke": false,
  "verify_vmaf": 91.8,
  "proxy_verify_gap": 0.612,
  "score_backend": "cuda"
}
```

## Exit Codes

| Code | Meaning |
| --- | --- |
| `0` | Recommendation produced and proxy/verify gap is within tolerance. |
| `2` | Argument or runtime setup error. |
| `3` | Recommendation emitted, but proxy/verify gap exceeded `--proxy-tolerance`; fall back to the slow grid. |

## Probe Extraction & Normalisation Pipeline

During the TPE search, each candidate trial executes a lightweight probe
encode and feature extraction pass:

1. **Probe Decode**: Probe encodes emit standard container bitstreams (e.g.
   `.mp4`). Before feature extraction, `maybe_decode_distorted` decodes the
   container to a temporary raw YUV file clamped to `--sample-chunk-seconds`.
   The temporary YUV is guaranteed to be cleaned up after extraction.
2. **Canonical-6 Feature Extraction**: libvmaf extracts canonical-6 features
   (`adm2`, `vif_scale0`..`vif_scale3`, `motion2`) using libvmaf pooled metric
   keys (`integer_adm2`, `integer_vif_scale0..3`, `integer_motion2`) with
   fallback to per-frame averages.
3. **StandardScaler Normalisation**: Raw feature averages are standardised via
   `(x - mean) / std` using `feature_mean` and `feature_std` statistics loaded
   from `model/tiny/fr_regressor_v2.json` before feeding the ONNX proxy regressor
   alongside the 14-D codec block (`ENCODER_VOCAB_V2`).
4. **Strict Error Propagation**: If a probe encode or feature extraction
   fails with a non-zero exit code, a `RuntimeError` is raised immediately.
   Probe failures are never masked with zero-filled features (`[0.0] * 6`),
   preventing corrupted search trials.
5. **Cross-Language Parity**: The Python extraction and normalisation contract
   matches the Go twin implementation in `pkg/fast` (`vmafx-tune fast`).
   `tools/vmaf-tune/tests/test_fast_parity.py` runs both implementations on
   the same clip and asserts identical raw pooled means and normalised
   features within 1e-6.

## See Also

- [`vmaf-tune.md`](vmaf-tune.md) — umbrella tool documentation.
- [`docs/ai/models/fr_regressor_v2.md`](../ai/models/fr_regressor_v2.md)
  — proxy model card.
- [ADR-0276](../adr/0276-vmaf-tune-fast-path.md) and
  [ADR-0304](../adr/0304-vmaf-tune-fast-path-prod-wiring.md).
