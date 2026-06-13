# TransNet V2 shot-boundary detector (100-frame window)

`transnet_v2` — a *shot-change* detector that consumes a 100-frame
sliding window of small RGB thumbnails and emits one shot-boundary
probability per frame. The first half of the Wave 1 §2.4 content-
adaptive encoding pipeline (the second half — per-shot CRF prediction —
is **T6-3b**, a follow-up that consumes these per-frame probabilities
through the existing feature collector).

> **Status — real upstream weights (T6-3a-followup).** As of
> [ADR-0261](../../adr/0261-transnet-v2-real-weights.md) the
> `model/tiny/transnet_v2.onnx` checkpoint ships verbatim trained
> weights from upstream
> [github.com/soCzech/TransNetV2](https://github.com/soCzech/TransNetV2)
> (Soucek & Lokoc 2020, MIT) wrapped in a thin NTCHW-input adapter.
> The original placeholder-only design is documented in
> [ADR-0223](../../adr/0223-transnet-v2-shot-detector.md).

## What the outputs mean

The extractor appends two per-frame features:

| Feature name | Type | Meaning |
| --- | --- | --- |
| `shot_boundary_probability` | float32 in `[0, 1]` | Sigmoid of the network's logit for the current frame. ~0.0 = no cut, ~1.0 = high-confidence cut. |
| `shot_boundary` | float32 ∈ `{0.0, 1.0}` | Binary flag thresholded at 0.5 against the probability. Drop-in for naive consumers. |

Downstream consumers (the per-shot CRF predictor T6-3b, the FFmpeg
shot-cut filter shipping with T6-3b) bind to those exact strings.

| Probability | Interpretation |
| --- | --- |
| **~0.05** | No shot change — typical mid-shot frame. |
| **~0.50** | Detector uncertain — common during dissolve / fade transitions and the first ~50 frames of warm-up. |
| **~0.95** | High-confidence shot cut. |

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model name | `vmaf_tiny_transnet_v2_v1` |
| Location | `model/tiny/transnet_v2.onnx` |
| Size | ~30 MiB (real upstream weights, ~7.7M parameters in the published checkpoint plus the ColorHistograms branch) |
| ONNX opset | 17 |
| Input | `frames` — float32 `[1, 100, 3, 27, 48]` (100-frame stack of RGB thumbnails, NTCHW) |
| Output | `boundary_logits` — float32 `[1, 100]` (per-frame logits before sigmoid) |
| Smoke flag | `smoke: false` in registry — real shot detector |
| License | MIT (upstream `soCzech/TransNetV2`) |
| Upstream commit | `77498b8e4a6d61ed7c3d9bd56f4de2b29ab7f4db` |
| TF SavedModel parity | max-abs-diff `< 4e-6` over 3 random `[0..255]` input trials |

The sidecar JSON at `model/tiny/transnet_v2.json` carries the input /
output names plus `frame_window: 100`, `thumbnail_h: 27`,
`thumbnail_w: 48`, `boundary_threshold: 0.5` so downstream consumers
can validate the contract without parsing the ONNX graph. Fresh exports add
ADR-0661 `run_provenance` with the upstream SavedModel paths, wrapped
SavedModel scratch path, parsed exporter arguments, ONNX output, sidecar
output, and registry target.

## Wrapper layer (NTCHW adapter)

Upstream's TensorFlow SavedModel takes
`[batch, frames, height, width, channels]` (NTHWC) and returns two
outputs: `output_1` (single-frame shot logits) and `output_2`
(auxiliary "many_hot" output trained against fades / dissolves). The
fork's C-side extractor (ADR-0223) declared an NTCHW input
`[1, 100, 3, 27, 48]` and a single `[1, 100]` logits output. The
exporter `ai/scripts/export_transnet_v2.py` wraps the upstream
SavedModel in a `tf.Module` whose forward pass:

1. transposes inputs from NTCHW → NTHWC (axes `0,1,2,3,4` → `0,1,3,4,2`),
2. invokes `base.signatures['serving_default']` with the upstream input,
3. selects only `output_1`,
4. squeezes the trailing singleton dim so downstream sees `[1, 100]`.

After tf2onnx conversion, one rank-2 `UnsortedSegmentSum` node in
upstream's `ColorHistograms` branch is rewritten as an equivalent
`ScatterND` reduction='add' subgraph (standard ONNX 17 doesn't ship
`SegmentSum`, and `tf2onnx` lowers `UnsortedSegmentSum` to a rank-1-
only op). The rewrite is numerically identical (no learned params
involved); see `_replace_segmentsum` in `ai/scripts/export_transnet_v2.py`.

## Op allowlist update

This PR extends `core/src/dnn/op_allowlist.c` with six new ops that
appear in the upstream TransNet V2 graph: `BitShift`, `GatherND`, `Pad`,
`Reciprocal`, `ReduceProd`, `ScatterND`. Each is a deterministic
standard ONNX op with bounded runtime cost (no control-flow, no host
allocation). Rationale + alternatives in
[ADR-0261](../../adr/0261-transnet-v2-real-weights.md).

## Frame window contract

The C extractor (`core/src/feature/transnet_v2.c`) maintains a
100-slot ring buffer of pre-resized RGB thumbnail tensors. Each
`extract()` call:

1. Resizes the input luma plane (any bpc; rescaled to `[0, 1]`) down
   to a 27x48 grid via nearest-neighbour, then broadcasts that single
   plane across all three RGB channels (placeholder behaviour
   preserved from ADR-0223 — true RGB decode + bilinear resize is
   tracked as a separate follow-up; the model accepts the broadcast
   luma since the upstream training data was natural-image RGB and
   the network is robust to per-channel correlation).
2. Pushes the resized frame into the ring at `next_slot`.
3. Gathers the 100 ring slots into a `[1, 100, 3, 27, 48]` input
   tensor. At clip start (when fewer than 100 frames have been seen)
   the missing slots replicate the oldest available frame
   (head-clamp).
4. Calls `vmaf_dnn_session_run` with the named bindings `frames`
   (input) and `boundary_logits` (output).
5. Reads the most recent slot's logit (index `WINDOW-1`), sigmoids
   it, and appends both `shot_boundary_probability` and
   `shot_boundary` (thresholded at 0.5) via
   `vmaf_feature_collector_append`.

The first ~50 frames of any clip should be treated as warm-up: the
detector hasn't seen enough context to make a confident decision.

## Integration recipe

```bash
# 1. Build libvmaf with DNN support enabled.
meson setup core/build-cpu -Denable_dnn=enabled
ninja -C core/build-cpu

# 2. Run the extractor against a clip, supplying the model path.
core/build-cpu/tools/vmaf \
    --reference ref.yuv --distorted dis.yuv \
    --width 1920 --height 1080 --pixel_format yuv420p --bitdepth 8 \
    --feature transnet_v2=model_path=model/tiny/transnet_v2.onnx

# Or via env var (matches lpips_sq / fastdvdnet_pre):
VMAF_TRANSNET_V2_MODEL_PATH=model/tiny/transnet_v2.onnx \
    core/build-cpu/tools/vmaf --feature transnet_v2 ...
```

The extractor declines cleanly (non-fatal `-EINVAL`) if neither
`model_path` nor `VMAF_TRANSNET_V2_MODEL_PATH` is set, the same
contract as the LPIPS and FastDVDnet extractors.

## Reproducing the export

```bash
# 1. Fetch upstream weights (LFS-tracked ~30 MiB).
git clone --depth=1 https://github.com/soCzech/TransNetV2.git \
    /tmp/transnetv2_upstream
git -C /tmp/transnetv2_upstream lfs pull \
    -I inference/transnetv2-weights

# 2. Verify upstream sha256 (the exporter also enforces this; bumping
#    UPSTREAM_COMMIT in the script is a deliberate weights swap).
sha256sum /tmp/transnetv2_upstream/inference/transnetv2-weights/saved_model.pb
# expect: 8ac2a52c5719690d512805b6eaf5ce12097c1d8860b3d9de245dcbbc3100f554
sha256sum /tmp/transnetv2_upstream/inference/transnetv2-weights/variables/variables.data-00000-of-00001
# expect: b8c9dc3eb807583e6215cabee9ca61737b3eb1bceff68418b43bf71459669367

# 3. Install conversion deps in a Python 3.11 venv (TF doesn't yet
#    publish wheels for Python 3.14).
python3.11 -m venv /tmp/transnet-venv
/tmp/transnet-venv/bin/python -m pip install \
    tensorflow tf2onnx onnx onnxruntime numpy

# 4. Export.
/tmp/transnet-venv/bin/python ai/scripts/export_transnet_v2.py \
    --upstream-dir /tmp/transnetv2_upstream/inference/transnetv2-weights
```

The exporter overwrites `model/tiny/transnet_v2.onnx`,
`model/tiny/transnet_v2.json`, and the matching `model/tiny/registry.json`
row; it also asserts `< 1e-4` max-abs-diff against the wrapped TF
SavedModel before declaring success.

## Smoke test

The C-side registration + options-table contract + dual-feature
surface is exercised by `core/test/test_transnet_v2.c`:

```bash
meson test -C core/build test_transnet_v2
```

To smoke the full 100-frame round-trip via Python ORT:

```bash
python3 -c "
import onnxruntime as ort, numpy as np
sess = ort.InferenceSession('model/tiny/transnet_v2.onnx',
                            providers=['CPUExecutionProvider'])
x = np.random.RandomState(7).rand(1, 100, 3, 27, 48).astype(np.float32)
y = sess.run(['boundary_logits'], {'frames': x})[0]
print('shape', y.shape, 'mean prob',
      float((1.0/(1.0+np.exp(-y))).mean()))
"
```

## Follow-ups

- **T6-3b**: per-shot CRF predictor consuming `shot_boundary_probability`
  per frame, plus shot-merge / min-length aggregation logic.
- **T6-3c**: switch the C-side resize from nearest-neighbour
  luma-broadcast to true bilinear RGB decode. Upstream was trained on
  bilinear-resized RGB, so the broadcast-luma path is a small loss
  of fidelity; quantifying it requires a labelled shot-boundary
  validation corpus we don't yet host.

## References

- Soucek, Lokoc. *TransNet V2: An effective deep network architecture
  for fast shot transition detection*, 2020.
  [arXiv:2008.04838](https://arxiv.org/abs/2008.04838).
- Reference implementation:
  [github.com/soCzech/TransNetV2](https://github.com/soCzech/TransNetV2)
  (MIT-licensed TensorFlow SavedModel).
- [ADR-0223](../../adr/0223-transnet-v2-shot-detector.md) — original
  design + placeholder-only PR.
- [ADR-0261](../../adr/0261-transnet-v2-real-weights.md) — this PR's
  decisions (NTCHW adapter, SegmentSum rewrite, op-allowlist
  extension).
- [Roadmap §2.4](../roadmap.md) — Wave 1 schedule.
- [ADR-0215](../../adr/0215-fastdvdnet-pre-filter.md) — sister
  placeholder-ONNX pattern (5-frame window FastDVDnet); its
  real-weights drop is [ADR-0255](../../adr/0255-fastdvdnet-pre-real-weights.md).
