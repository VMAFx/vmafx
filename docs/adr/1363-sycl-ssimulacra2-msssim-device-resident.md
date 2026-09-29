<!-- markdownlint-disable MD013 MD060 -->
# ADR-1363: The SYCL ssimulacra2 twin is device-resident, and float_ms_ssim_sycl waits once per frame

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, gpu, ssimulacra2, ms-ssim, performance, numerics, rc3, fork-local

## Context

`ssimulacra2_sycl` ([ADR-0206](0206-ssimulacra2-cuda-sycl.md)) blurred on the
device and did everything else on the host. Per frame it converted YUV to linear
RGB on the CPU, and for each of the six scales it computed XYB on the CPU,
uploaded both XYB images, ran the five blurs, copied five full-size three-plane
buffers (`mu1`, `mu2`, `s11`, `s22`, `s12`) back, waited, and ran the SSIM and
edge-difference combine and the 2x2 downsample on the CPU. At 3840x2160 that is
about 4 GB of device-host traffic and six host waits per frame; the copies were
also sized for the full frame at every scale. The host combine made the twin
bit-identical to the CPU extractor, because it evaluated the per-pixel terms in
fp64 and summed them in the CPU's order.

`float_ms_ssim_sycl` enqueued its per-scale horizontal and vertical passes in
`collect()` and waited on the queue after each scale (five times per plane) to
read that scale's per-work-group l/c/s partials, although nothing needs them
before all scales are done.

The maintainer's requirement for RC3 is that there be no GPU/CPU round trips per
frame ("there shouldnt be any gpu cpu rountrips" / "remove the host roundtrips
and then test again"). The device has no fp64 ([ADR-0220](0220-sycl-fp64-fallback.md)).

## Decision

**ssimulacra2_sycl** becomes a submit/collect extractor whose whole frame runs
on the device in one in-order chain. `submit()` packs the six raw Y/U/V planes
into pinned staging and uploads them; the device converts YUV to linear RGB,
and per scale computes XYB, the three products and five blurs, six
per-channel sums of the per-pixel SSIM and
edge-difference terms, and the downsample. One 864-byte copy of the per-scale
sums is the only readback; `collect()` waits once and forms the 108 norms and
the pooled score in fp64 with the CPU extractor's formulas.

- The TU is compiled with contraction off (`sycl_exact_fp_args` /
  `sycl_exact_fp_sources` in `core/src/meson.build`, the SpEED flag of
  [ADR-1358](1358-sycl-speed-device-resident-linalg.md) renamed for its second
  user), products that feed adds sit in named temporaries, and the cube root
  of `ssimulacra2_math.h` divides through `vmaf_sycl_exact::div_rn()` via a new
  `VMAF_SS2_FDIV` hook (the host default is the plain operator). The sRGB EOTF
  is the shared LUT function itself. With that, YUV conversion, XYB, the
  blurs and the downsample are bit-identical to the CPU.
- The per-pixel terms that the CPU evaluates in fp64 — `1 - num_m * num_s /
  denom_s`, `(1 + |r2 - mu2|) / (1 + |r1 - mu1|) - 1` and the fourth powers —
  are evaluated in exact fp32 pairs (relative error about 2^-44), and summed in
  a fixed tree: a fixed strided subset per work-item, a fixed local-memory
  tree per work-group, then one work-group per channel over the group
  partials. The tree depends only on the plane size, so the result is
  deterministic and identical on every device.
- The pair arithmetic and the correctly rounded division move from
  `speed_sycl_pipeline.cpp` to `core/src/feature/sycl/sycl_exact_fp.h`, shared
  by both TUs (HISS-19), with `ff_div()` added.

The CPU sums 8.3 million fp64 terms per plane at 4K one after another; no
parallel device reduction can reproduce that sequence of roundings, so the
twin is no longer bit-identical to the CPU. The measured maximum difference in
the per-frame score is 6.7e-12 at 3840x2160, 1.1e-12 at 576x324 and 6.3e-13 at
853x481 (4:4:4), identical on the Arc B580 and the UHD 770, against the
ADR-0214 tolerance of 5e-3. The bound follows from the pair precision: the
per-pixel terms are within about 1e-14 of the fp64 ones, the tree sum carries
no more error than the CPU's sequential sum, and the fourth roots and the
polynomial pool keep relative errors of that size.

**float_ms_ssim_sycl** keeps its kernels and arithmetic. Each (plane, scale)
writes its partials to its own span of one buffer, `submit()` enqueues the
pyramids and every scale's passes plus one copy of the whole buffer, and
`collect()` waits once and sums each span in the order the per-scale readback
used. Its output is bit-identical to the previous SYCL output.

The CUDA, HIP and Metal ssimulacra2 twins keep their host combine until the same
chain is ported; each is an RC3 row in `docs/state.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the host combine; copy each scale into its own host buffer and wait once | Bit-identical to the CPU as before; smallest change | Still about 4 GB of device-host traffic per 4K frame, host XYB, host downsample and a host combine of 8.3 M pixels; not "no round trips" | Does not meet the requirement; the traffic is most of the cost |
| Device combine in plain fp32 with `sycl::reduction` | Simplest device code | `1 - q` with q near 1 cancels; fp32 terms carry about 6e-8 relative error per pixel and the reduction order is unspecified, so scores would move run to run and device to device | Trades determinism and parity for speed |
| Emulate fp64 exactly on the device and sum sequentially | Could reproduce the CPU bit for bit | Sequential over 8.3 M terms per plane in software double; seconds per frame | Far too slow, and a device fp64 path is what ADR-0220 forbids |
| Exact fixed-point (integer) accumulation | Order-independent and exact | The fourth powers span more than 60 binades (d^4 down to 1e-24 and below); a fixed-point word loses them or needs a multi-word superaccumulator | Pairs keep relative precision at every magnitude for a fraction of the cost |
| Form ref^2, dis^2 and ref*dis inside the horizontal blur pass (no separate multiply) | Saves the multiply pass and its buffer | The row walk is bound by its per-lane loads on the UHD 770, and a product doubles them. Horizontal passes per 4K frame, fused -> separate: UHD 770 373 -> 239 ms plus 16 ms of multiply; B580 11.3 -> 10.0 ms plus 2.3 ms | Kept a separate coalesced multiply (UHD 770 -118 ms, B580 +1 ms) |
| Horizontal blur pass staged through local memory (contiguous row loads, then a lane-per-row walk over the tile) | Should cut the per-lane loads of the row walk | Measured per stage: 10.0 -> 20.5 ms per 4K frame on the B580 and 239 -> 1328 ms on the UHD 770 | Kept the lane-per-row walk over global memory |
| **Device-resident chain, exact pairs, fixed-tree sums (chosen)** | No host compute or wait mid-frame, one small readback; deterministic and device-independent; within 1e-11 of the CPU | Not bit-identical to the CPU any more (it was); more device code | Chosen |

## Consequences

- **Positive**: ssimulacra2 per frame, before -> after: 963 -> 33 ms at
  3840x2160 and 27.1 -> 3.3 ms at 576x324 on the Arc B580, 1025 -> 445 ms and
  39.1 -> 20.7 ms on the UHD 770 (the CPU extractor on 16 threads: 167 and
  2.3 ms). float_ms_ssim is unchanged in value and saves the per-scale waits:
  14.4 -> 13.9 ms (B580) and 155 -> 138 ms (UHD 770) per 4K frame; the 576x324
  differences are inside the timing method's noise. Full table in
  [Research-1363](../research/1363-sycl-ssimulacra2-msssim-device-resident.md).
  The twin no longer depends on the host C library for `fmaf` (the device FMA
  is always single-rounded; the ADR-1205 issue cannot recur on this path).
- **Negative**: ssimulacra2_sycl moves from bit-identical to within about 1e-11
  of the CPU. The UHD 770 stays slower than the CPU extractor at 4K: the
  horizontal IIR walk alone takes 239 of its 427 ms of device time per frame
  (Research-1363, finding 7), and two alternatives measured slower.
  `ssimulacra2_sycl` now rejects 4:0:0 input at init (the CPU extractor and the
  old twin read a chroma plane that does not exist); its ADR-1324 context check
  routes that input, and frames below 8x8, to the CPU extractor when the twin
  was picked by a model or by the ADR-1359 `--backend` mapping.
- **Neutral / follow-ups**: port the chain to CUDA, HIP and Metal
  (`T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29`,
  `T-HIP-SSIMULACRA2-HOST-COMBINE-2026-09-29`,
  `T-METAL-SSIMULACRA2-HOST-COMBINE-2026-09-29`); `integer_motion_sycl` still
  waits on the queue in `submit()` when `motion_add_uv` is set
  (`T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29`). The Arc A380 calibration
  entry for ssimulacra2 in `scripts/ci/gpu_ulp_calibration.yaml` (places=1)
  predates this chain and can be tightened once the A380 is measured.
  `core/test/test_sycl_kernel_source_contract.py` guards the shared header's
  fp64-freeness, the contraction-off build of every TU that uses it, the
  absence of the retired host stages and the single collect-time wait of both
  extractors.

## References

- `req` (maintainer, 2026-09-29): "there shouldnt be any gpu cpu rountrips" /
  "remove the host roundtrips and then test again".
- [ADR-0206](0206-ssimulacra2-cuda-sycl.md) (the hybrid this replaces for SYCL),
  [ADR-1358](1358-sycl-speed-device-resident-linalg.md) (contraction-off TUs,
  `div_rn`, exact pairs), [ADR-1357](1357-sycl-cambi-device-resident.md),
  [ADR-0220](0220-sycl-fp64-fallback.md), [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0985](0985-sycl-parity-divergence-2026-06-03.md),
  [ADR-1205](1205-ssimulacra2-fma-unification-scalar-and-gpu.md),
  [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md).
- [Research-1363](../research/1363-sycl-ssimulacra2-msssim-device-resident.md).
