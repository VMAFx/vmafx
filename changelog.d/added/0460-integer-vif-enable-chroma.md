### `integer_vif`: add `enable_chroma` option

`integer_vif` now accepts `enable_chroma` (bool, default `false`). When enabled,
the four-scale VIF pipeline runs on the Cb and Cr planes in addition to luma and
emits `integer_vif_scale{0..3}_cb` / `..._cr` per-frame scores. For YUV400
(monochrome) input the option is silently clamped to `false`.

This mirrors the identical option already present on `psnr` and `ssim`
(ADR-0453). Luma scores are numerically identical to prior behaviour.
