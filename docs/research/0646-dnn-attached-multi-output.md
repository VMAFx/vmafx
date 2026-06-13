# Research Digest: Attached DNN Multi-Output Routing

## Question

Can `vmaf_use_tiny_model()` publish multiple scalar ONNX outputs without adding
a new public C API or changing existing single-output score names?

## Findings

- Standalone `vmaf_dnn_session_run()` already accepts multiple inputs and
  outputs via caller-owned `VmafDnnOutput` buffers.
- The attached path used `vmaf_ort_infer()`, which resolves only the first
  session output and returns one scalar buffer. That made the ONNX session
  usable for single-head models only.
- The feature collector can already store multiple feature names per frame. It
  does not need a new append-many primitive if the attached bridge derives a
  stable collector key for each output and appends each scalar separately.
- Sidecar JSON is the existing runtime metadata channel for tiny models. Adding
  optional `output_names[]` there avoids public API churn and keeps model-owned
  output labels next to the ONNX bundle.
- Single-output compatibility is load-bearing: changing `fr_regressor_v2` to
  emit `fr_regressor_v2_score` would break downstream report consumers that
  look up the historical `fr_regressor_v2` key.

## Implementation Shape

- Parse optional sidecar `output_names[]` into `VmafModelSidecar`.
- Prepare attached collector keys once at attach time:
  - single-output: keep `feature_name`;
  - multi-output: use count-matched sidecar labels, else ONNX output names;
  - sanitize output suffixes and de-duplicate fallbacks.
- Run attached rank-2 and rank-4 models through `vmaf_ort_run()` with one output
  slot per graph output.
- Keep attached mode scalar-only; vector/image outputs remain standalone-session
  territory.

## Validation

```bash
docker exec vmaf-dev-mcp bash -lc 'cd /workspace && rm -rf /tmp/vmaf-dnn-multi-output-build && meson setup /tmp/vmaf-dnn-multi-output-build libvmaf -Denable_dnn=enabled -Denable_cuda=false -Denable_sycl=false -Denable_vulkan=disabled -Denable_hip=false -Denable_metal=disabled && meson test -C /tmp/vmaf-dnn-multi-output-build --suite=dnn --print-errorlogs'
```

Result: 12/12 DNN tests passed before the documentation closeout. The new
regression attaches `model/tiny/smoke_multi_output_v0.onnx`, feeds a 4x4 luma
frame, and asserts both `multi_probe_mean_score` and `multi_probe_peak_score`
are present in the feature collector.
