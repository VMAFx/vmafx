- **CAMBI spatial-mask rows use SIMD on x86 and aarch64**: the per-frame
  summed-area (`compute_dp_row`) and box-sum threshold (`compute_mask_row`)
  rows of CAMBI's spatial mask now dispatch to AVX2, AVX-512 and — for the dp
  row — NEON kernels, adapted from upstream Netflix/vmaf `86da14d03`. On a Zen 5
  core at 1080p the dp row runs 1.8–2.3x faster with AVX2 and 2.6–3.2x with
  AVX-512, and the mask row 1.5–1.7x and 2.1–2.3x, depending on the compiler.
  The kernels are bit-exact, so CAMBI scores are unchanged. The upstream AVX2 dp
  row was slower than scalar under Clang and icx, so the fork's version carries
  the running prefix through a single add per block instead
  ([ADR-1256](docs/adr/1256-cambi-spatial-mask-simd-dispatch.md)). See
  [CAMBI CPU SIMD paths](docs/metrics/cambi.md#cpu-simd-paths).
