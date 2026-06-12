# `vmaftune.predict` — Python API reference

The `vmaftune.predict` module exposes the per-shot VMAF predictor class used
by `vmaf-tune recommend`, `vmaf-tune ladder`, and the auto-driver pipeline.

For the conceptual overview (purpose, signal pipeline, validation contract),
see [ai/predictor.md](../ai/predictor.md).

## `Predictor`

```python
class Predictor:
    def __init__(
        self,
        encoder: str,
        preset: str | None = None,
        model_path: str | None = None,
    ) -> None: ...
```

Loads the per-codec ONNX predictor for `encoder` (e.g. `"libx264"`,
`"libsvtav1"`) and optional `preset`. `model_path` overrides the built-in
model registry lookup. Raises `FileNotFoundError` when no ONNX model is
registered for the given `(encoder, preset)` pair and no override is provided.

### `predict_vmaf`

```python
def predict_vmaf(
    self,
    signals: ProbeSignals,
    crf: int,
) -> float:
```

Returns the predicted VMAF score (in the 0–100 range) for a shot encoded at
`crf` given the probe signals `signals`. This is a **point estimate**
(the predictor's mean prediction); it is identical whether or not the
uncertainty-aware pipeline is active.

For interval predictions, see
[`ConfidenceThresholds`](../ai/conformal-vqa.md) and
`vmaftune.uncertainty.predict_with_intervals`.

### `predict_vmaf_batch`

```python
def predict_vmaf_batch(
    self,
    signals_list: Sequence[ProbeSignals],
    crf: int,
) -> list[float]:
```

Batch variant of `predict_vmaf`. Submits all signals to the ONNX runtime in
a single inference call; more efficient than looping over
`predict_vmaf` for large shot lists.

## `ProbeSignals`

```python
@dataclass
class ProbeSignals:
    bitrate_kbps: float          # probe-encode output bitrate
    intra_ratio: float           # fraction of I-frames in the probe
    inter_ratio: float           # fraction of P/B-frames
    mean_qp: float               # mean QP from the probe stream
    saliency_score: float = 0.0  # optional; 0.0 = saliency-unaware path
```

Signal bundle extracted from a probe encode. All fields are scalar floats.
`saliency_score` is the output of `vmaftune.saliency.score_frame_avg` for the
shot; callers that do not use saliency must leave it at the default `0.0`.

The codec adapter in `vmaf-tune` populates this automatically from the
ffprobe output stream — callers of the library API receive a
`ProbeSignals` object from `run_probe_encode(...)` and pass it directly to
`predict_vmaf`.

## Typical usage

```python
from vmaftune.predict import Predictor, ProbeSignals
from vmaftune.codec import run_probe_encode

predictor = Predictor(encoder="libx264", preset="medium")

for shot in shots:
    probe = run_probe_encode(shot, crf=28, encoder="libx264", preset="medium")
    predicted_vmaf = predictor.predict_vmaf(probe.signals, crf=crf)
    if predicted_vmaf >= target:
        scheduled_crf = crf
        break
```

## See also

- [ai/predictor.md](../ai/predictor.md) — conceptual overview, validation
  contract, and training provenance
- [usage/vmaf-tune-recommend.md](../usage/vmaf-tune-recommend.md) — CLI
  reference for the per-clip CRF-search consumer
- [ai/conformal-vqa.md](../ai/conformal-vqa.md) — uncertainty-aware interval
  extension
- [api/ladder.md](ladder.md) — related bitrate ladder Python API
