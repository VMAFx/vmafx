<!-- markdownlint-disable MD013 MD060 -->
# Research-1363: Device-resident ssimulacra2 and a single-wait MS-SSIM on SYCL

- **Status**: Active
- **Workstream**: [ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md), [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md), [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md), [ADR-0220](../adr/0220-sycl-fp64-fallback.md)
- **Last updated**: 2026-09-29

## Question

The maintainer asked for no GPU/CPU round trips per frame ("there shouldnt be
any gpu cpu rountrips"). `ssimulacra2_sycl` blurred on the device and did the
rest of every scale on the host; `float_ms_ssim_sycl` waited on the queue after
each scale. Can both keep the frame on the device, and what does that cost in
parity with the CPU extractor on a device without fp64?

## Sources

- `core/src/feature/ssimulacra2.c` (CPU reference: `picture_to_linear_rgb`,
  `linear_rgb_to_xyb`, `fast_gaussian_1d` / `blur_plane`, `multiply_3plane`,
  `ssim_map`, `edge_diff_map`, `downsample_2x2`, `pool_score`) and its AVX2
  twins in `core/src/feature/x86/ssimulacra2_avx2.c`, which accumulate the
  per-pixel fp64 terms in the same sequential order.
- The pre-change `core/src/feature/sycl/ssimulacra2_sycl.cpp` and
  `integer_ms_ssim_sycl.cpp`; the CUDA, HIP and Metal ssimulacra2 twins.
- [Research-1358](1358-sycl-speed-device-resident.md) for the contraction and
  division findings on icpx 2026.1.
- `vmaf-dev-mcp:ocloc` (icpx 2026.1, AOT for `bmg-g21` and `adl-s`), Arc B580
  and UHD 770 through WSL2 Level Zero, i9-12900K for the CPU runs.
  Fixtures: the Netflix 576x324 pair (48 frames), 50 frames of BBB 3840x2160,
  and an odd-size pair (the Netflix pair scaled to 853x481 4:4:4, 48 frames;
  the CLI rejects odd widths for 4:2:0).

## Findings

1. **The old twin was bit-identical to the CPU, and paid for it in traffic.**
   All 146 per-frame scores matched `--backend cpu` on both devices, because
   the host evaluated the per-pixel terms in fp64 and summed them in the CPU's
   order. Per scale it uploaded two and downloaded five three-plane buffers
   sized for the full frame, whatever the scale: about 4 GB of copies per 4K
   frame, plus host YUV conversion, host XYB and a host combine over 8.3 M
   pixels. The B580 took about 0.96 s per 4K frame, slower than the CPU
   extractor with 16 threads.
2. **Everything except the sums can be made bit-identical on the device.**
   With the TU compiled contraction-off (the SpEED flag of ADR-1358), products
   in named temporaries, `sycl::fma` for the YUV matrix, the shared
   `vmaf_ss2_srgb_eotf` (its LUT is a constant-initialised global, usable from
   device code) and `vmaf_ss2_cbrtf` with its division routed to `div_rn`
   (`VMAF_SS2_FDIV`), YUV conversion, XYB, the multiplies, both IIR passes and
   the downsample reproduce the CPU exactly. Evidence: after the move, the
   per-frame differences are at the 1e-12 level on every fixture, the size of
   the sum error alone; any per-pixel drift upstream of the sums grows to
   1e-3 or more (ADR-1205).
3. **The sums cannot be bit-identical.** The CPU adds 8.3 M fp64 terms per
   plane at 4K one after another; the result depends on every intermediate
   rounding. A parallel device reduction cannot replay that sequence, and
   software fp64 on one work item would take seconds per frame (and ADR-0220
   forbids fp64 in SYCL kernels). An exact integer accumulator does not fit
   either: the fourth powers span more than 60 binades.
4. **Exact fp32 pairs in a fixed tree keep the score within 1e-11.** The
   product `num_m * num_s` of two floats is exact as a pair (`two_prod`), the
   quotient is formed with `ff_div` (two correctly rounded partial quotients,
   relative error about 2^-46), `1 - q` and the edge ratio
   `(1 + |r2 - mu2|) / (1 + |r1 - mu1|) - 1` stay in pairs, and the fourth
   powers are pair products. Six sums per channel go through a fixed tree:
   a fixed strided pixel subset per work-item, a local-memory tree per
   work-group, one group per channel over the partials. The tree depends only
   on the plane size, and every operation is correctly rounded, so the result
   is deterministic and identical on the B580 and the UHD 770.
5. **Measured parity** (per-frame `ssimulacra2`, `--precision max`, against
   `--backend cpu --threads 16`):

   | Fixture | Before (host combine) | After (device sums) |
   |---|---|---|
   | Netflix 576x324, 48 frames | 48/48 identical | max abs diff 1.1e-12 |
   | 853x481 4:4:4, 48 frames | 48/48 identical | max abs diff 6.3e-13 (1 identical) |
   | BBB 3840x2160, 50 frames | 50/50 identical | max abs diff 6.7e-12 |

   B580 and UHD 770 give identical scores. The ADR-0214 tolerance is 5e-3.
6. **MS-SSIM needed only a buffer per scale.** The per-scale wait existed
   because one partials buffer was reused across scales. With a span per
   (plane, scale), every scale is enqueued in `submit()` and `collect()` waits
   once; the host sums each span in the old group order. Every output
   (default, `enable_lcs`, `enable_chroma`) is identical to the previous SYCL
   build on all fixtures and both devices.
7. **Where the device time goes** (final design, SYCL event profiling,
   ms per frame):

   | Stage | B580 4K | UHD 770 4K | B580 576x324 | UHD 770 576x324 |
   |---|---|---|---|---|
   | YUV -> linear RGB | 0.84 | 13.8 | 0.02 | 0.39 |
   | XYB | 1.43 | 28.2 | 0.07 | 0.77 |
   | Three multiplies | 2.31 | 15.7 | 0.03 | 0.38 |
   | Horizontal blur (5 per scale) | 10.05 | 239.0 | 1.05 | 6.89 |
   | Vertical blur (5 per scale) | 9.31 | 65.3 | 0.90 | 4.25 |
   | SSIM / edge sums | 2.90 | 59.4 | 0.20 | 1.76 |
   | Downsample | 0.79 | 5.2 | 0.03 | 0.22 |
   | Total | 27.6 | 426.7 | 2.28 | 14.65 |

   The IIR is a sequential recurrence along each line, so a pass has one
   work-item per row or column. The vertical walk reads adjacent addresses
   across lanes; the horizontal walk makes every lane of a load touch its own
   cache line, which the UHD 770 pays for most.
8. **Two blur variants that lost.** Forming the products inside the
   horizontal pass (saves the multiply pass) doubles that pass's per-lane
   loads: horizontal passes 373 ms per 4K frame on the UHD 770 against 239 ms
   plus 16 ms of multiply when separate (B580 11.3 against 10.0 + 2.3 ms).
   Staging each group's rows through local memory with contiguous loads and
   walking the tile was slower still: 20.5 ms on the B580 and 1328 ms on the
   UHD 770 for the horizontal passes. Both kept the scores unchanged.
9. **Wall time** (median ms per frame, `(t(N) - t(2)) / (N - 2)`, N = 22 at
   4K and 48 at 576x324, runs interleaved before/after, medians of 5 at 4K
   and 7 at 576x324):

   | Feature | Size | CPU, 16 threads | B580 before | B580 after | UHD 770 before | UHD 770 after |
   |---|---|---:|---:|---:|---:|---:|
   | `ssimulacra2` | 576x324 | 2.27 | 27.11 | 3.25 | 39.05 | 20.70 |
   | `ssimulacra2` | 3840x2160 | 166.86 | 963.27 | 33.30 | 1024.64 | 444.93 |
   | `float_ms_ssim` | 576x324 | 1.29 | 1.46 | 0.81 | 5.18 | 4.16 |
   | `float_ms_ssim` | 3840x2160 | 58.63 | 14.38 | 13.92 | 154.65 | 138.01 |

   Each run is timed inside the shared GPU lock, so waiting for other GPU
   users is not counted. GPU runs use `--backend sycl --feature
   <feature>_sycl` with the default `--threads`; CPU runs use `--backend cpu
   --threads 16`.

   At 576x324 the per-frame cost of `float_ms_ssim` is within the start-up
   noise of the method; the medians are listed for completeness.
10. **Audit of the other SYCL twins.** Every other twin waits once per frame,
    in `collect()` or `vmaf_sycl_graph_wait()`, plus `flush()` drains, a
    profiling-only `ev.wait()` in `vif_sycl` and the single end-of-frame
    readback of `psnr_hvs_sycl`. One exception: `motion_sycl` with
    `motion_add_uv=true` waits on the queue inside `submit()` after uploading
    U and V (`motion_upload_chroma`), so the combined graph on another queue
    does not read them early (`T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29`).
11. **The CUDA, HIP and Metal ssimulacra2 twins have the same host combine**
    (`ss2c_host_combine`, `ss2h_host_combine`, `ss2m_host_combine`), with host
    YUV conversion, host XYB, host downsample and a wait per scale. Each is an
    RC3 row in `docs/state.md` with the port reference and a verify command.

## Alternatives explored

- **Host combine with one wait per frame** (a host buffer per scale). Keeps
  bit-exactness but not the requirement: the 4 GB of copies and the host
  compute stay.
- **Plain fp32 device sums with `sycl::reduction`.** `1 - q` with q close to 1
  cancels in fp32, and the reduction order is unspecified: scores would vary
  run to run and device to device.
- **Blur variants** in finding 8.

## Open questions

- The horizontal IIR walk dominates on the UHD 770 (239 of 427 ms per 4K
  frame); a formulation with contiguous loads that does not lose to the plain
  walk has not been found. The CPU extractor on 16 threads remains faster than
  the UHD 770 at 4K.
- The Arc A380 (fp64-less Alchemist) was not measured. Its ssimulacra2 entry
  in `scripts/ci/gpu_ulp_calibration.yaml` (places=1) predates this chain.

## Related

- ADR-1363, ADR-1358, ADR-1357, ADR-0206, ADR-0220, ADR-0214, ADR-1205.
- `docs/state.md`: `T-SYCL-SSIMULACRA2-MSSSIM-HOST-ROUNDTRIPS-2026-09-29`,
  `T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29`,
  `T-HIP-SSIMULACRA2-HOST-COMBINE-2026-09-29`,
  `T-METAL-SSIMULACRA2-HOST-COMBINE-2026-09-29`,
  `T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29`,
  `T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29`.
