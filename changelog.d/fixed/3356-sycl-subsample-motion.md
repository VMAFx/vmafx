- **Run active SYCL combined-graph extractors on subsampled frames (#3356).**
  `vmaf_sycl_graph_submit()` now counts and enqueues only the registered graph extractors that run
  on the current frame under `--subsample N` (`N > 1`), using direct per-extractor submission on
  frames where non-temporal extractors (`vif_sycl`, `adm_sycl`, `float_moment_sycl`, `cambi_sycl`)
  are skipped so temporal `motion_sycl` and `psnr_sycl` update every frame without overwriting
  pending host results of skipped extractors.
