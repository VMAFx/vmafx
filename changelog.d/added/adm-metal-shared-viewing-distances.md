- **`integer_adm_metal` evaluates two viewing distances in one instance
  (ADR-2795).** With `adm_norm_view_dist_extra`, or when two models such as
  `vmaf_v1.0.16_3d0h` and `_5d0h` run on `--backend metal`, the Metal twin
  runs the wavelet transform once per scale and the other stages per
  distance. Its host replay returns the CPU's scores for both distances bit
  for bit; the Apple-device run comes from the macOS tester bundle
  ([Two viewing distances share one `adm`](docs/metrics/adm.md#two-viewing-distances-share-one-adm)).
