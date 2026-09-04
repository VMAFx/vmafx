- **sycl:** Fix crashes and prediction errors when running default model `vmaf_v1.0.16_3d0h`
  on Intel Arc GPUs. Fix uninitialized bounds and histogram buffer allocation in
  `integer_cambi_sycl.cpp`, eliminate `double` accumulators and accessors in
  `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` (ADR-0220 fp64-less contract),
  and add `cambi_high_res_speedup` (`hrs`) option to `options_cambi_sycl` to resolve
  feature dictionary naming mismatch (`-EAGAIN`). Also pass `-fp-model=precise` to
  AVX2/AVX-512 static libraries under `icx` ensuring CPU golden parity. (ADR-1179)
