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

## See Also

- [`vmaf-tune.md`](vmaf-tune.md) — umbrella tool documentation.
- [`docs/ai/models/fr_regressor_v2.md`](../ai/models/fr_regressor_v2.md)
  — proxy model card.
- [ADR-0276](../adr/0276-vmaf-tune-fast-path.md) and
  [ADR-0304](../adr/0304-vmaf-tune-fast-path-prod-wiring.md).
