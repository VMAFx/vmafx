- **vmaf-roi-score**: Make `--synthetic-mask` materialise and score a
  real constant-mask YUV instead of re-scoring the unmodified distorted
  input. The smoke path now exercises the mask pipeline without
  requiring ONNX Runtime.
