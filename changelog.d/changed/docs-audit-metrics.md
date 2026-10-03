- Corrected the metric and model pages against the extractor option tables
  and the exact-twin declarations. `features.md` is now the feature index: one
  coverage table of every registered extractor with its GPU twins and their
  exactness, then the option reference; ADM, CIEDE2000, float moment, SpEED and
  the tiny-AI extractors have their own pages. Corrected: `vif_enhn_gain_limit`
  defaults to 100.0, integer `motion` `debug` defaults to false, `motion_v2`
  has seven options, `speed_chroma` emits u, v and uv, the five-frame motion
  window reads frames n-3, n-1 and n+1, and the default model is
  `vmaf_v1.0.16_3d0h`.
