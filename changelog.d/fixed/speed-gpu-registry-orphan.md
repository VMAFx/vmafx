- The SpEED GPU twins (`speed_{chroma,temporal}_{cuda,sycl,hip}`) are
  registered again, so `vmaf_get_feature_extractor_by_name("speed_chroma_cuda")`
  (and the SYCL / HIP variants) resolve and the GPU kernels actually run.
  PR #875 split `feature_extractor.c` into the compiled `feature_extractor.cpp`
  but left the six GPU SpEED `extern`s + registry entries behind in the now-dead
  `.c`, so the kernels compiled yet were unreachable by name and SpEED silently
  fell back to the CPU path (ADR-0964 / ADR-0965 / ADR-0852). The dead
  `feature_extractor.c` twin is deleted to remove the split-brain footgun, and
  `test_feature_extractor` now asserts every GPU SpEED twin resolves by name
  under its backend. Other GPU twins (`adm`/`vif`/`cambi`/… `_cuda`/`_sycl`/
  `_hip`) were unaffected — they were already present in the `.cpp` registry.
