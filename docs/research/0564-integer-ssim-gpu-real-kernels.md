<!-- markdownlint-disable MD013 MD060 -->
# Research digest: Real integer_ssim GPU kernels (ADR-0564)

## Correctness investigation

The root cause was traced by diffing the `provided_features` field across the three
GPU backend extractors registered for `"ssim"`:

- `integer_ssim_cuda.c` (pre-ADR-0564): `provided_features = {"float_ssim", NULL}`.
  The kernel was `ssim_score.cu`, an 11-tap floating-point Gaussian.
- `integer_ssim_hip.c` (pre-ADR-0564): state struct held five `float *` device buffers
  (`d_ref_mu`, `d_cmp_mu`, etc.) — the float_ssim intermediate layout.
- `integer_ssim_sycl.cpp` (pre-ADR-0564): no `vmaf_fex_integer_ssim_sycl` symbol at all;
  the lookup silently fell through to the CPU twin.

In every case, calling `vmaf_use_feature("ssim", NULL)` on a GPU backend dispatched
either the wrong metric or the CPU fallback.

## CPU algorithm analysis

The CPU integer_ssim (`integer_ssim.c`) computes per-pixel moments using a 9-tap
Gaussian with integer weights `[2,9,28,55,68,55,28,9,2]` (sum=256). It uses `int64_t`
throughout the accumulation to avoid floating-point rounding. The SSIM formula:

```text
c1 = (K1 * samplemax * w)^2     // w = per-pixel accumulated weight
c2 = (K2 * samplemax * w)^2
num = (2*mux*muy + c1) * (2*(xy*w - mux*muy) + c2)
den = (mux^2 + muy^2 + c1) * (x2*w - mux^2 + y2*w - muy^2 + c2)
ssim = w * num / den
```

is computed in `double` after the int64 moment accumulation. Boundary-truncation
(not clamping) means pixels near the edge use a reduced kernel; `w` accumulates
only the in-bounds tap weights.

## CUDA two-pass design rationale

Separating horizontal and vertical passes avoids large shared-memory tiles. Each
pass processes one 16×8 block:

- Pass 1 writes six `int64_t` arrays of size `W×H` (one per moment: mux, muy, x2, xy, y2, w).
- Pass 2 reads those arrays, applies the 9-tap vertical kernel, computes the SSIM formula
  in `double`, and accumulates per-block double partial sums.
- The host accumulates all partial sums and divides by the sum of partial weights.

Warp reduction uses paired int32 shuffles for int64 lane values (pre-sm70 compatible
because `__shfl_down_sync` is used on 32-bit halves of the 64-bit value).

**Parity result (CUDA)**: all 48 frames of the Netflix 576×324 8bpc golden fixture
produce diff=0.00e+00 vs CPU. Pooled mean ssim: 0.86138600 (CPU) = 0.86138600 (CUDA).

## HIP wavefront-64 design

The HIP kernel (`integer_ssim_score.hip`, pre-existing from ADR-0533) uses wavefront-64
shuffles (GCN/RDNA) with `ISSIM_WARP_SIZE=64`. Int64 lane sums use `issim_warp_sum_i64`
which packs hi/lo into paired int32 shuffles — analogous to the CUDA paired-shuffle
approach.

The host glue rewrite (`integer_ssim_hip.c`) changes:

- 5 `float *` buffers → 6 `int64_t *` device buffers
- Readback: two slots (double partials, int64 weights)
- `collect`: `ssim = total_ssim / (double)total_wgt`

## SYCL fp64-free constraint

Intel Arc A380 Level Zero rejects entire SPIR-V modules that use `fp64` inside
kernel lambdas (as opposed to merely not advertising the `fp64` capability — the
driver treats the entire `.spv` as invalid). ADR-0220 documents this.

The SYCL extractor uses `float` for the SSIM formula inside the kernel. Int64 moment
accumulation is retained (int64 is supported). The int64 moments are converted to
`float` before the SSIM formula step. This introduces at most 1 ULP of rounding vs
the CPU's double formula, limiting agreement to places=4–5.

`reduce_over_group` aggregates both `float d_partials` and `int64_t d_wgt` across
work groups. The host divides `sum(float_partials)` by `(double)sum(int64_wgt)`.

## Accepted precision targets

| Backend | Algorithm | Expected precision vs CPU |
|---------|-----------|--------------------------|
| CUDA | int64 moments + double SSIM | places=6 (confirmed: diff=0) |
| HIP | int64 moments + double SSIM | places=6 (hardware gate pending) |
| SYCL | int64 moments + float32 SSIM | places=4–5 (fp64-free constraint, ADR-0220) |

## Files touched

- `core/src/feature/cuda/integer_ssim/integer_ssim_score.cu` (new)
- `core/src/feature/cuda/ssim_cuda.c` (new)
- `core/src/feature/cuda/ssim_cuda.h` (new)
- `core/src/feature/hip/integer_ssim_hip.c` (rewritten)
- `core/src/feature/sycl/integer_ssim_sycl.cpp` (appended extractor)
- `core/src/feature/feature_extractor.c` (extern + list entries)
- `core/src/meson.build` (new PTX and C source entries)
