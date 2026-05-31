- `core/src/dnn/ort_backend.c`: corrected the `VMAF_DNN_DEVICE_AUTO` fallback
  comment to reflect the actual execution-provider chain
  (CUDA → OpenVINO:GPU → ROCm → CoreML → CPU); the previous text erroneously
  stated an OpenVINO:CPU step that does not exist in the AUTO path.
