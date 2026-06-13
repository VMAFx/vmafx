<!-- markdownlint-disable MD060 -->
# Research-0654: Predictor Saliency Signal Wiring

## Summary

The signal-mix audit follow-up found a concrete saliency gap in
`vmaf-tune`'s per-shot predictor path. `ShotFeatures` and the shipped
predictor ONNX input layout already reserve saliency and signalstats
slots, but two code paths failed to populate them:

- `predictor_features._compute_saliency(...)` called
  `vmaftune.saliency.compute_saliency_map(...)` with obsolete keyword
  arguments, caught the resulting exception, and returned `(0.0, 0.0)`.
- `predictor_train.project_row(...)` always zero-filled saliency and
  signalstats columns, even for corpus rows that already carried those
  richer features.

## Options

| Option | Result |
|---|---|
| Keep zero-filled rows until a new predictor schema exists | No ONNX compatibility risk, but all refreshed corpora still discard saliency. |
| Introduce a new predictor schema | Clean contract, but invalidates existing 14-column model/card/sidecar expectations. |
| Preserve the existing 14-column schema and populate the reserved slots | Minimal compatibility risk and immediately unlocks richer corpora. |

## Finding

The third option is the right scope. The predictor layout already has
`saliency_mean`, `saliency_var`, `frame_diff_mean`, `y_avg`, and
`y_var`; using those columns does not widen the ONNX input. Runtime
saliency needs a temporary raw-YUV decode because the public
`predict --source` accepts any FFmpeg-readable container while
`compute_saliency_map(...)` operates on raw `yuv420p`.

## Action

Patch `predict --use-saliency` to decode the shot range to temporary
`yuv420p`, call the current saliency helper with `(raw_path, width,
height, model_path=..., frame_samples=...)`, and feed the aggregate mean
and variance into `ShotFeatures`. Patch `predictor_train.project_row` to
preserve row-provided probe-byte, saliency, and signalstats columns,
falling back to the historical stand-ins only when a row is legacy.
