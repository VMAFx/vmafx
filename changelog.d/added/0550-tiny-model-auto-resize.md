- **`--tiny-resize {bilinear,nearest,bicubic,disabled}` CLI flag** plus
  matching public C API `vmaf_dnn_set_resize_mode(ctx, mode)` and
  `VmafDnnResizeMode` enum
  (`core/include/libvmaf/dnn.h`): select the resize filter the NCHW
  tiny-model dispatch uses when the user-supplied frame dims don't
  match the model's expected input shape (e.g. 576x324 YUV against
  the 224x224 `nr_metric_v1.onnx` NR scorer). Default is `disabled`
  (dim mismatch → -ERANGE; operator must opt in explicitly).
  Pass `--tiny-resize bilinear` (or `nearest`/`bicubic`) to enable
  auto-resize. Note: the three filters produce scores differing by
  ~2% on the same input — document filter choice with the model.
  ADR-0550.
- **`vmaf_tensor_from_luma_resize()`**
  (`core/src/dnn/tensor_io.{c,h}`): separable nearest /
  bilinear / Catmull-Rom-bicubic resampling helper with
  replicate-edge clamping. Bit-identical to `vmaf_tensor_from_luma`
  on the no-resize fast path. ADR-0550.

### Fixed

- **NR tiny-model 0-frames footgun (post-fix probe Finding 11)**:
  `vmaf --no-reference --tiny-model nr_metric_v1.onnx --distorted
  576x324.yuv ...` previously hard-errored at frame 0 with `-ERANGE`,
  bubbled up through `vmaf_read_pictures` as "problem reading
  pictures", and produced an empty `frames` array with a silent
  footer. The per-frame NCHW dispatch now supports auto-resampling
  when `--tiny-resize bilinear` (or similar) is explicitly passed;
  with `--tiny-resize bilinear` on 576x324 input: 48 frames + real
  mean score. ADR-0550.
