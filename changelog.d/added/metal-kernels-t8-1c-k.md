# Metal kernel batch T8-1c through T8-1k — seven remaining real kernels (ADR-0421)

Promotes the seven remaining Metal feature-extractor scaffolds from
`-ENOSYS` stubs to functional MSL kernels, following the `integer_motion_v2`
template established in T8-1c.

### What ships

- **`float_psnr.metal`** / **`float_psnr_metal.mm`** (T8-1d): per-pixel
  `(ref - dis)^2` float accumulation. 8bpc and 16bpc kernel variants.
  Host: `psnr = min(10*log10(peak^2 / mse), psnr_max)`. Feature: `float_psnr`.

- **`float_moment.metal`** / **`float_moment_metal.mm`** (T8-1e): per-pixel
  ref/dis 1st and 2nd moment sums. 4 float partials per threadgroup
  (interleaved ref1st/dis1st/ref2nd/dis2nd). Also **fixes the scaffold's
  wrong `provided_features`** (`float_moment1/2/std` → correct names
  `float_moment_ref1st`, `float_moment_dis1st`, `float_moment_ref2nd`,
  `float_moment_dis2nd` matching all other backends). Features:
  `float_moment_ref1st`, `float_moment_dis1st`, `float_moment_ref2nd`,
  `float_moment_dis2nd`.

- **`float_ansnr.metal`** / **`float_ansnr_metal.mm`** (T8-1f): 3×3 ref +
  5×5 dis convolution with shared-memory mirror-padded 20×20 tile. Per-WG
  (sig, noise) float partial pair; host: `ansnr = 10*log10(sig/noise)`.
  Feature: `float_ansnr`.

- **`integer_psnr.metal`** / **`integer_psnr_metal.mm`** (T8-1g): per-pixel
  `(int64)(ref - dis)^2` SSE accumulated via `uint` partials (lo/hi
  uint slots per WG, host stitches into uint64). Three separate dispatches
  (one per plane). Features: `psnr_y`, `psnr_cb`, `psnr_cr`.

- **`float_motion.metal`** / **`float_motion_metal.mm`** (T8-1h): temporal
  5-tap Gaussian blur (`FILTER_5_s`) + float SAD vs previous blurred frame.
  Skip-boundary mirror (diverges from motion_v2). State holds `prev_blurred`
  MTLBuffer. Feature: `float_motion`.

- **`integer_motion.metal`** / **`integer_motion_metal.mm`** (T8-1i): integer
  motion (v1) — 5-tap filter on raw integer pixels, per-WG `uint` SAD
  partials. Skip-boundary mirror. Features: `VMAF_integer_feature_motion_y_score`,
  `motion2_score`, `motion3_score`.

- **`float_ssim.metal`** / **`float_ssim_metal.mm`** (T8-1j): two-pass SSIM
  (horizontal then vertical 11-tap Gaussian, then SSIM formula). Stores
  intermediate blurred planes in MTLBuffers allocated at init. Features:
  `float_ssim`, `float_ms_ssim`.

### Key design note: no `atomic_ulong`

All kernels use per-WG `float`/`uint` partials arrays — **not** MSL
`atomic_fetch_add_explicit` for 64-bit types, which fails silently on
Apple Silicon (CI run 25685703780 / job 75408804495). Host reduces in
`double`. See `docs/research/0421-metal-kernel-batch-t8-1c-k.md`.

### Build (macOS only)

```bash
meson setup build libvmaf -Denable_metal=enabled \
    -Denable_cuda=false -Denable_sycl=false
ninja -C build
meson test -C build test_metal_smoke
```

### Parity gate

```bash
scripts/ci/cross_backend_parity_gate.py \
    --feature float_psnr,float_moment,float_ansnr,psnr_y,float_motion,float_ssim \
    --backends cpu,metal --places 3
```
