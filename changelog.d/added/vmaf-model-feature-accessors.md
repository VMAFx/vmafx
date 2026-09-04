- `vmaf_model_feature_count` and `vmaf_model_feature_name` added to the
  public C API (`<libvmaf/model.h>`). Callers can inspect the features
  required by a loaded `VmafModel` without accessing opaque struct internals.
