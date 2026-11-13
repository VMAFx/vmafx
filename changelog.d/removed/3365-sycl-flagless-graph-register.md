- **SYCL**: removed the internal flagless `vmaf_sycl_graph_register()`
  compatibility entry point. `vmaf_sycl_graph_register_flags()` is now the
  only combined-graph registration function, so every extractor registers
  with its own `VmafFeatureExtractor` flags.
