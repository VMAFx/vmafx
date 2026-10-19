---
paths:
  - core/src/feature/sycl/ssimulacra2_sycl.cpp
  - core/src/feature/sycl/sycl_ssimulacra2_math.h
  - core/test/test_sycl_ssimulacra2_parity.c
invariant: ssimulacra2_sycl = CPU ssimulacra2, bit for bit; device-resident; IIR recurrence has no running accumulator.
---
<!-- markdownlint-disable MD013 MD060 -->
# SSIMULACRA2 perceptual metric

- **`ssimulacra2_sycl.cpp` IIR recurrence has no running accumulator —**
  **never 'Kahan' it** (ADR-0985). Charalampidis recursive blur = 3-pole
  autoregressive IIR filter
  ($o_k = n2 \cdot \text{sum} - d1 \cdot \text{prev1} - \text{prev2}$), not
  cumulative summation. Adding accumulator term $\text{prev1}$ into
  output shifts poles outside unit circle ($1 - d1 \approx -0.8422$),
  causing geometric pole blow-up to $10^{25}$ / NaN / saturation at 100.0.
  Recurrence must remain pure float32 matching CUDA twin
  `core/src/feature/cuda/ssimulacra2/ssimulacra2_blur.cu`. places=1
  (`5.0e-2`) Arc A380 calibration of that time is gone: twin exact since
  ADR-1446.
- **`ssimulacra2_sycl.cpp` is device-resident
  ([ADR-1363](../../../../../docs/adr/1363-sycl-ssimulacra2-msssim-device-resident.md)).**
  `submit()` uploads six raw planes and enqueues whole frame (YUV ->
  linear RGB, per scale XYB, three products and five blurs, SSIM / edge
  sums, downsample) plus one copy of per-scale
  sums; `collect()` is only wait. Load-bearing: TU builds with
  contraction off (every feature TU does, ADR-1367); `VMAF_SS2_FDIV` maps shared
  `vmaf_ss2_cbrtf` division to `div_rn` before `ssimulacra2_math.h` is
  included; products feeding adds sit in named temporaries; YUV / XYB /
  blur / downsample are bit-identical to `ssimulacra2.c` and parity sweep
  proves it. sums of per-pixel fp64 terms of `ssim_map` /
  `edge_diff_map` are CPU's too (ADR-1446, next bullet).
  `check_context_sycl` routes inputs init rejects
  (4:0:0, side below 8) to CPU `ssimulacra2` (ADR-1324 / ADR-1359); keep
  it in step with `init_fex_sycl`. **On rebase**: do not reintroduce host
  stage or mid-frame wait (`test_sycl_kernel_source_contract.py` fails),
  and keep
  per-channel sum order (L1, L4, artifact, artifact^4, detail, detail^4) that
  `ss2s_scale_norms` reads. Two blur variants measured slower (ADR-1363):
  products fused into horizontal pass (2.3x on UHD 770, which is bound
  by per-lane loads of row walk) and rows staged through local
  memory (2x on B580, 5.6x on UHD 770); do not retry either without
  measuring per stage.
- **`ssimulacra2_sycl` = CPU `ssimulacra2`, bit for bit
  ([ADR-1446](../../../../../docs/adr/1446-sycl-ssimulacra2-cpu-bits.md)).**
  CPU: six fp64 terms per sample and channel, each added pixel after pixel
  into ONE double. Twin, no fp64 type:
  - Terms = `sycl_ssimulacra2_math.h` (`ssim_terms()`, `edge_terms()`):
    reference's fp64 operations one for one on `SoftSigned`
    (`sycl_soft_signed.h`), from fp32 values reference converts
    (`ss2s_ssim_inputs()` = fp32 part verbatim). Returns fp64 bit patterns.
    Zero denominator: quotient = infinity of product's sign, clamped
    `d` = 0. Value reference makes infinite or NaN -> NaN bits (sum keeps
    it, frame guard rejects, as CPU). Zero has no sign.
  - Sums = `sycl_ordered_sum.h` on `../ordered_sum.h` with
    `VMAF_ORDSUM_NO_FP64` (`_bits` forms only). Chunk = `SS2S_CHUNK` (512)
    consecutive pixels = one work-group, 256 lanes x 2 CONSECUTIVE pixels.
    Per scale: `launch_chunk_sums` (fp32 pair terms, fp32 tree: ADVICE only;
    fourth powers scaled by 2^88) -> `launch_chunk_plan`
    (`plan_chunks()`, one work-item per sum) -> `Ss2SsimUnitsKernel` /
    `Ss2EdgeUnitsKernel` (exact terms -> integer increments under plan,
    composed in pixel order by `ss2s_ordered_tree`, lane `i` takes lane
    `i + stride`) -> `Ss2SlotKernel` (chunks planned "term by term": keep
    terms, compose runs of 16 under two binades chunk ends in)
    -> `Ss2TotalsKernel` (`walk_sum()`, ONE lane per sum).
  - Rules: (1) advice sums never reach result: plan only; (2) plan =
    advice: walk adds chunk or run from its increment only when
    `vmaf_ordsum_add_chunk_bits()` accepts it at exact sum, never drop
    that check; (3) composition (`vmaf_ordsum_then`) NOT commutative: earlier
    pixels on left, no halving tree, no strided lanes; (4) terms >= 0 or
    NaN only; (5) change to terms in `ssimulacra2.c` ->
    `sycl_ssimulacra2_math.h` + `reference_terms()` of its test, same PR;
    (6) `ordered_sum.h` is shared with CUDA and HIP: fp64 forms stay
    wrappers of `_bits` forms, no `double` outside
    `#ifndef VMAF_ORDSUM_NO_FP64`.
  - Why slots and runs: one A380 lane takes about 1 us per fp64 add in
    integers; crossing chunk walked term by term = 512 such steps. Runs
    make it about 32 integer steps + 16 terms. Wrong advice, no slot, wrong
    expected binade = slower, same bits.
  - Shapes (scratch-free on A380, ADR-1395): units SIMD-16 default register
    file, slot kernel SIMD-16 + 256-entry file, walk SIMD-16 (8 until
    ADR-1468: Xe2 does not compile it). SIMD-32 spills;
    chunk 1024 spills in slot kernel without large file (wrong
    values, runs that hang past 300 s).
  - Guards: `test_sycl_ssimulacra2_math` (terms vs reference lines, host +
    device), `test_sycl_ordered_sum` (sum vs loop, wrong plans, host +
    device walk), `test_sycl_ssimulacra2_parity` + `_large` (`==`; 15 of 16
    score cases differ on old twin),
    `test_sycl_ssimulacra2_exact_contract.py`, `test_sycl_kernel_scratch`.
    Cost + tuning candidates:
    `T-SYCL-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02`.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `ssimulacra2_sycl.cpp` | `ssimulacra2.c` | `test_sycl_ssimulacra2_parity.c` (+ `_large`; bit-exact, 8 to 16 bit, 4:2:0 / 4:2:2 / 4:4:4), `test_sycl_ssimulacra2_math.c`, `test_sycl_ordered_sum.c` | ADR-0957 (round 4), ADR-1363, ADR-1446 |
