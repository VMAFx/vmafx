---
paths:
  - core/src/feature/sycl/integer_psnr_hvs_sycl.cpp
  - core/test/test_sycl_psnr_hvs_parity.c
invariant: psnr_hvs_sycl = CPU scores bit for bit; integer_psnr_hvs_sycl.cpp DCT lives in local memory.
---
<!-- markdownlint-disable MD013 MD060 -->
# PSNR-HVS extractor and DCT kernel

- **`integer_psnr_hvs_sycl.cpp` DCT lives in local memory, never in one
  work-item's private arrays** (T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29,
  Research-2123). Old shape: work-item 0 ran whole 8x8 DCT + masking on
  five private 64-element arrays; IGC 2.41.5 SIGSEGVs host compiling that
  at SIMD32 for Xe2 (Arc B580). Now (ADR-1369, Research-1369): two work-items
  per block (2k ref, 2k + 1 dist, same sub-group), each with 65-word local
  slot (pad = distinct banks): `hvs_load_block()` raw samples ->
  `hvs_variance_ratio()` -> `hvs_fdct8x8()` in place (columns stored as
  columns = CPU `z` transposed, then rows) -> `hvs_mask_energy()` ->
  `hvs_threshold()`; `permute_group_by_xor(.., 1)` exchanges two
  thresholds, `group_barrier(sg)`, ref item runs `hvs_store_terms()`. One
  dispatch for all planes, terms laid out `[Y | Cb | Cr]` (`first_block[]`,
  64 floats per block). On rebase: no private `int[64]` / `float[64]`; do
  not "fix" by pinning `VMAF_SYCL_REQD_SG_SIZE(16)` (on UHD 770
  compiler's own choice beats forced SIMD16 by 1.7x). Samples are read raw
  at every depth (old `sample_to_int` x16 for 9/11 bits is gone).
  Guards: `test_sycl_psnr_hvs_parity{,_simd32,_large}`,
  `test_sycl_shared_planes`.
- **`psnr_hvs_sycl` = CPU scores bit for bit (ADR-1397, ADR-1401).** Kernel
  stores 64 terms `calc_psnrhvs()` sums per block (`hvs_store_terms`,
  row-major, blocks in plane then raster order); `reduce_hvs_planes()` hands
  each plane to `vmaf_psnr_hvs_plane_score()` (`../psnr_hvs_score.c`: one
  running `float`, CPU order), combined score + dB via same file.
  Load-bearing, each one breaks bit-identity on its own (Research-1401):
  - masking table = `(csf * 0.3885746225901003)^2` in `double`, stored
    `float` (`hvs_mask_value` / `MASK_TABLES`, constexpr: host compiler
    evaluates it, kernel reads static data, stays fp64-free);
  - threshold = `sqrt_rn(energy * ratio) / 32.f` (ADR-1488): fp32 product,
    correctly rounded fp32 root = CPU's float product and double root
    (rounding root to 53 bits, then 24 = rounding to 24; checked on all
    2 139 095 039 positive floats). Not exact product (`sqrt_prod_rn()`,
    removed: fork's CPU between PR #552 and ADR-1488), not bare
    `sycl::sqrt` (not `sqrt_rn()`: correct only while flag line holds);
  - coefficient error = integer `sycl::abs()` cast to `float`;
  - strict FP line (ADR-1367): no contraction, correctly rounded `/`
    (`threshold / mask`, variance ratio);
  - no sum of terms in TU (per-block partials round differently:
    1e-2 dB at 3840x2160).
  Kernel stays scratch-free (ADR-1395): `private_mem_size` and
  `spill_memory_size` 0 on A380 at SIMD16 and forced SIMD32; terms go
  straight to USM, no private `float[64]`; `test_sycl_kernel_scratch` fails
  if kernel gains any. `n_active_planes` = 1 for `enable_chroma=false`
  or 4:0:0 (CPU `psnr_hvs.c::init` rule, set in `configure_hvs_geometry()`),
  else 3; 4:0:0 must not reach shared chroma planes. Guards:
  `test_sycl_psnr_hvs_parity{,_simd32,_large}` (device, `==` on all four
  outputs, 3840x2160 included), `test_sycl_fp_arith_contract`
  (`sqrt_rn` of fp32 product on device vs host's statement),
  `test_psnr_hvs_twin_exact_sum_contract.py` (device-free). Gate cell =
  tolerance 0 (`EXACT_TWINS`). Upstream change to `calc_psnrhvs()`
  arithmetic or order -> mirror it here in same PR. Readback = 256 bytes
  per block (65 MB per 3840x2160 4:2:0 frame); tuning tracked as
  T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01, must stay bit-exact.
- **`integer_psnr_hvs_sycl.cpp` uses ceiling division for chroma plane
  geometry** (PR #1031). `init_fex_sycl` computes 4:2:0 / 4:2:2 chroma
  `width[1..2]` / `height[1..2]` via `(w + 1U) >> 1` / `(h + 1U) >> 1`, not
  `w >> 1` / `h >> 1`, to match `picture.c` / CPU `integer_psnr_hvs.c` /
  CUDA + HIP twins on odd-dimension YUV420 / YUV422. Floor division drops
  last chroma 8x8 block strip on odd dimensions, diverging `psnr_hvs_cb` /
  `psnr_hvs_cr` / `psnr_hvs` from every other backend (even dimensions
  unaffected). On rebase: picture allocator's ceiling subsample convention
  (`(dim + ss) >> ss`) = single source of truth. Any new SYCL extractor
  re-deriving plane dims in its own `init` must use ceiling form. Any
  upstream change to chroma-dimension formula propagates here and to
  CUDA + HIP twins in same PR.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_psnr_hvs_sycl.cpp` | `third_party/xiph/psnr_hvs.c` | `test_sycl_psnr_hvs_parity.c` | ADR-0946 (round 3) |

- Prefix scan (`launch_scan_prefix()`) visits every chunk of 256 blocks: `for (c = 0;
  c < num_chunks; ...)`, like CUDA twin. cap (was `32768u`) leaves
  later chunk offsets unset and compaction writes past its buffer: more
  than 32768 chunks = 8,388,608 blocks = 16384x8640 4:4:4, right past 16K
  (256,779 chunks at 32768 cap). Guard:
  `test_psnr_hvs_gpu_scan_contract.py`
  (T-GPU-PSNR-HVS-SCAN-32768-CHUNKS-2026-10-05).
