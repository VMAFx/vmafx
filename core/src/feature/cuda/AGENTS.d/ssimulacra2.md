---
paths:
  - core/src/feature/cuda/ssimulacra2_cuda.c
  - core/src/feature/cuda/ssimulacra2_cuda.h
invariant: ssimulacra2_cuda is device-resident and computes bit-exact CPU scores.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Device-resident SSIMULACRA2

- **`ssimulacra2_cuda` is device-resident** (ADR-1391, CUDA port of
  ADR-1363 SYCL chain). `submit()` enqueues whole frame on picture
  stream (YUV -> linear RGB from device planes, then per scale XYB, five
  blurs, per-pixel SSIM / edge sums, downsample) and one 864-byte copy of
  per-scale sums; `collect()` waits once and pools on host. Invariants:
  - No host compute and no host wait inside frame: no `cuStreamSynchronize`,
    no DtoH / HtoD of planes. `core/test/test_cuda_ssimulacra2_parity.c` holds
    twin to `==` with CPU on every frame (ADR-1433).
  - `ssimulacra2_blur` and `ssimulacra2_device` build with `--fmad=false`
    (every fatbin does, ADR-1403); products that feed
    add stay in their own expressions; cube root divides through
    `VMAF_SS2_FDIV` = `__fdiv_rn`; YUV matrix uses `__fmaf_rn` in
    ADR-0891 order. That keeps YUV, XYB, blurs and downsample bit-identical
    to `ssimulacra2.c`.
  - `ssimulacra2_device.cu` compiles shared helpers of
    `feature/ssimulacra2_math.h`, `ssimulacra2_score.h` and
    `ssimulacra2_eotf_lut.h` into device code through `VMAF_SS2_FUNC` /
    `VMAF_SS2_EOTF_LUT_STORAGE` hooks. change to those headers changes
    CUDA twin; keep them valid device code.
  - Sums = CPU's loops, bit for bit (ADR-1433). `ssim_map()` /
    `edge_diff_map()` add each term pixel after pixel into one double;
    `feature/ordered_sum.h` gives that loop's bits from parallel pieces.
    Chunk = `SS2C_CHUNK_PIXELS` (1024) pixels in raster order = one block,
    256 lanes x `SS2C_CHUNK_RUN` (4) CONSECUTIVE pixels. Four kernels:
    `ssimulacra2_chunk_sums` (tree sums, advice only) ->
    `ssimulacra2_chunk_plan` (binade per chunk, from prefix of tree sums) ->
    `ssimulacra2_chunk_units` (integer increments, even/odd start, composed
    in pixel order: lane run, then `ss2c_ordered_tree`, lane `i` takes lane
    `i + step`) -> `ssimulacra2_ordered_totals` (lane 0 walks chunks; chunk
    failing `vmaf_ordsum_add_chunk` = 1024 terms added one by one, pixel
    order). Rules: (1) never store tree sum as total; (2) increment
    composition NOT commutative: no halving tree, no atomics, no strided
    lanes; (3) plan = advice, walk checks plan at exact sum, never drop that
    check; (4) terms >= 0 or NaN only (`d` clamped, edge split); (5) change to
    terms in `ssimulacra2.c` -> `ss2c_terms()` same PR. Guards:
    `test_ordered_sum` (host, 4 mutations fail),
    `test_cuda_ssimulacra2_exact_contract.py`, `test_cuda_ssimulacra2_parity`
    (`==`, fails on tree sum). Cost + tuning candidates:
    `T-CUDA-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-01`.
  - Blur layout: horizontal pass stages 32-row x 32-column tiles through
    shared memory with one warp per block and forms `ref^2`, `dis^2` and
    `ref*dis` on load (radius at most `SS2C_BLUR_MAX_RADIUS` = 16);
    vertical pass walks one column per thread on row-major output, which
    is already coalesced. ADR-0456 transpose and separate multiply kernel
    are gone: that layout measured about 2x slower
    ([Research-1391](../../../../../docs/research/1391-cuda-ssimulacra2-device-resident.md)).
