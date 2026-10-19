- **Two ADM viewing distances share one extractor (Netflix/vmaf `33e5f0aca`,
  `cffd5b77d`).** When two models need `adm` with options that differ only in
  `adm_norm_view_dist` (for example `vmaf_v1.0.16_3d0h` and `_5d0h`), libvmaf
  runs one `adm`: the wavelet transform and decouple once per scale, the
  weighting per distance, with every score bit-identical to separate runs. The
  new option `adm_norm_view_dist_extra` (`nvde`) requests a second distance
  directly; `adm_rust` does the same. Unlike upstream, a third model at the
  second distance is absorbed instead of failing the run, and a `debug`
  context keeps its scores
  ([Two viewing distances share one `adm`](docs/metrics/adm.md#two-viewing-distances-share-one-adm)).
