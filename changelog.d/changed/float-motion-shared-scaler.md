- The CUDA, SYCL and HIP `float_motion` twins take their bit-depth scaler from
  one helper, `vmaf_float_motion_scaler()` in `core/src/feature/float_motion_sad.h`,
  instead of three copies; scores are unchanged.
