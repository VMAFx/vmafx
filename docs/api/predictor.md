# `vmaftune.predictor`: Python API reference

Use `vmaftune.predictor` to estimate the VMAF of a shot at a given CRF from
cheap probe-encode signals, and to invert that estimate into the CRF that
meets a target VMAF, without running VMAF on the final encode. It is the
predict half of the predict-then-verify loop; `vmaftune.predictor_validate`
is the verify half. The source is
[`tools/vmaf-tune/src/vmaftune/predictor.py`](../../tools/vmaf-tune/src/vmaftune/predictor.py).

For the model, training and the production-flip gate see
[the predictor guide](../ai/predictor.md). Interval prediction is described
in [conformal VQA](../ai/conformal-vqa.md).

!!! note
    The navigation entry for this page reads `vmaftune.predict`; the module
    is `vmaftune.predictor`.

## Quick start

This program runs without onnxruntime or a model file: `Predictor()` uses
the built-in analytical curve.

```python
from vmaftune.predictor import Predictor, ShotFeatures

features = ShotFeatures(
    probe_bitrate_kbps=3200.0,
    probe_i_frame_avg_bytes=48000.0,
    probe_p_frame_avg_bytes=9000.0,
    probe_b_frame_avg_bytes=4000.0,
    shot_length_frames=240,
    fps=24.0,
    width=1920,
    height=1080,
)
predictor = Predictor()

vmaf = predictor.predict_vmaf(features, 28, "libx264")   # CRF 28
crf = predictor.pick_crf(features, 93.0, "libx264")      # largest CRF with VMAF >= 93
keyint, min_keyint = predictor.pick_keyint(features, 24.0)
```

With these inputs `predict_vmaf` returns about 93.96, `pick_crf` returns 28
and `pick_keyint` returns `(48, 12)`.

## `ShotFeatures`

A frozen dataclass of cheap signals for one shot. Each is read from the
probe encode's output or from a single centre frame; none needs VMAF or the
final encode. All values are non-negative.

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `probe_bitrate_kbps` | float | required | Average bitrate of the probe encode. |
| `probe_i_frame_avg_bytes` | float | required | Mean I-frame size. |
| `probe_p_frame_avg_bytes` | float | required | Mean P-frame size. |
| `probe_b_frame_avg_bytes` | float | required | Mean B-frame size; 0 when the codec has none. |
| `saliency_mean` | float | `0.0` | Mean saliency in `[0, 1]` (saliency student model). |
| `saliency_var` | float | `0.0` | Saliency variance across the centre frame. |
| `frame_diff_mean` | float | `0.0` | Mean absolute frame difference (motion energy). |
| `y_avg` | float | `0.0` | Mean luma over the shot. |
| `y_var` | float | `0.0` | Luma variance. |
| `shot_length_frames` | int | `0` | Shot length in frames. |
| `fps` | float | `0.0` | Frame rate. |
| `width`, `height` | int | `0` | Frame size. |

`resolution_class(height)` maps a height to the ladder vocabulary: `"sd"`
(up to 480), `"hd_ready"` (up to 720), `"hd"` (up to 1080), `"uhd"` (up to
2160) and `"uhd8k"` (everything above 2160).

## `Predictor`

```python
Predictor(model_path: Path | None = None, coefficients: Mapping[str, tuple[float, ...]] = ...)
```

A dataclass. There is no encoder or preset argument: the codec is chosen
per call.

| Argument | Effect |
| --- | --- |
| `model_path=None` | Analytical fallback only. |
| `model_path=<predictor_<codec>.onnx>` | Loads the learned per-codec model on the CPU execution provider. |
| `coefficients` | Per-codec `(a, b, c, d, crf_ref)` for the analytical curve; defaults cover 14 codecs (libx264, libx265, libsvtav1, libaom-av1, libvvenc and the NVENC, AMF and QSV H.264, HEVC and AV1 encoders). |

Behaviour to know:

- **No silent substitution when onnxruntime is missing.** If onnxruntime
  cannot be imported, the predictor uses the analytical curve. If it can be
  imported and `model_path` does not exist, construction raises
  `FileNotFoundError`; an invalid graph propagates its own error.
- **Stub models.** `is_stub` (read-only) is `True` when the model file name
  contains `stub`, when its card (`<name>_card.md`) says `synthetic-stub`, or,
  unless the card says `real-N=`, when the name contains a codec that ships
  only a synthetic model (libx264, libx265, libsvtav1, libaom-av1, libvvenc,
  h264_amf, hevc_amf, av1_amf). Loading a stub emits a `UserWarning`: such
  models are not authoritative for production CRF picks.
- **Analytical curve.** $\mathrm{vmaf} = a - b\,\delta - c\,\delta^2 + d \log_{10}(\mathrm{bitrate\_kbps})$
  with `delta = crf - crf_ref` and the codec's constants, bitrate floored at
  1 kbps and the result clamped to `[0, 100]`. An unknown codec uses the
  libx264 constants. The constants are seed values for tests, not trained
  results.

### Methods

| Method | Returns | Does |
| --- | --- | --- |
| `predict_vmaf(features, target_quality, codec)` | `float` in `[0, 100]` | Predicted VMAF at CRF `target_quality` (an int) for `codec`. Uses the ONNX model when loaded, else the analytical curve. |
| `predict_mos(features, codec, *, target_quality=None)` | `float` in `[1, 5]` | Predicted MOS. Uses `model/konvid_mos_head_v1.onnx` when it ships and onnxruntime imports; otherwise `(predict_vmaf - 30) / 14`, clamped, an approximation. `target_quality=None` uses the codec's default CRF. |
| `pick_crf(features, target_vmaf, codec)` | `int` | Binary search over the codec adapter's `quality_range` for the largest CRF whose predicted VMAF is at least `target_vmaf`. Returns the adapter's default CRF if none qualifies. |
| `pick_keyint(features, fps)` | `(keyint, min_keyint)` | GOP heuristic, below. |
| `predict_vmaf_with_uncertainty(features, target_quality, codec, *, calibration=None, alpha=None)` | `(point, low, high)` | Point estimate with a conformal interval. `calibration` is a `SplitConformalCalibration` or `CVPlusConformalCalibration` from `vmaftune.conformal`; `None` returns `(point, point, point)`. `alpha` overrides the calibration's miscoverage level. |

The interval covers the true value with probability at least `1 - alpha`
under exchangeability (split conformal, Lei et al. 2018; CV+, Barber et al.
2021).

### GOP heuristic

`pick_keyint(features, fps)` returns `(keyint, min_keyint)` from the probe
bitrate (kbps) and the shot length:

| Condition | `keyint` | `min_keyint` |
| --- | --- | --- |
| Shot of at least 4 s and probe bitrate below 1500 kbps | $4 \cdot \mathrm{fps}$ | `fps` |
| Probe bitrate above 8000 kbps | `fps` | $\max(\mathrm{fps} / 2, 1)$ |
| Otherwise | $2 \cdot \mathrm{fps}$ | $\max(\mathrm{fps} / 2, 1)$ |

`fps` is rounded to an integer first (at least 1). The thresholds are
constants chosen for 1080p natural content. The learned model picks the CRF
on top of these bands.

## Module functions

| Function | Does |
| --- | --- |
| `pick_crf(predictor, features, target_vmaf, codec)` | Function form of `Predictor.pick_crf`. |
| `pick_keyint(features, fps)` | Function form of `Predictor.pick_keyint`. |
| `make_predictor_predicate(predictor, feature_extractor)` | Adapts a predictor to the `PredicateFn` seam of `vmaftune.per_shot.tune_per_shot`: `(shot, target_vmaf, encoder) -> (crf, predicted_vmaf)`. `feature_extractor(shot, encoder)` returns a `ShotFeatures`; the real one lives in `vmaftune.predictor_features`, tests inject a stub. |

```python
from vmaftune.predictor import Predictor, make_predictor_predicate

predicate = make_predictor_predicate(Predictor(), my_feature_extractor)
crf, predicted_vmaf = predicate(shot, 93.0, "libx264")
```

## Uncertainty thresholds

`vmaftune.uncertainty.ConfidenceThresholds` turns an interval width into a
decision: `tight_interval_max_width` (default 2.0 VMAF) and
`wide_interval_min_width` (default 5.0 VMAF). See the
[ladder API](ladder.md#uncertainty-aware-recipe) for how the ladder uses
them and [conformal VQA](../ai/conformal-vqa.md) for the calibration.
