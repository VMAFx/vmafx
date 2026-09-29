<!-- markdownlint-disable MD013 MD060 -->
# ADR-1370: float_ssim_sycl decimates on the device, bit-identical to the CPU

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, gpu, ssim, performance, numerics, gpu-parity, rc3, fork-local

## Context

CPU `float_ssim` picks a decimation factor from the short side, `max(1, round(min(w, h) / 256))`: 1 below 384 px, 4 at 1920x1080, 8 at 3840x2160. `ssim.c` then low-passes both planes with a `scale x scale` box of weight `1.0f / (scale * scale)` and decimates them with `iqa_decimate()` before `iqa_ssim()`. `float_ssim_sycl` implemented scale 1 only, so the ADR-1324 context check refused every picture with a short side of 384 px or more, and model dispatch and `--backend sycl --feature float_ssim` (ADR-1359) computed the feature on the CPU with the warning "float_ssim_sycl cannot run 3840x2160 8-bit pictures with these options". The twin also copied and normalised both planes on the host (`picture_copy()`) and uploaded them as fp32, four bytes per 8-bit sample. RC3 owns the device path ([ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1352](1352-rc-phase-shift-plus-one.md)).

Constraints: SYCL kernels are fp64-free ([ADR-0220](0220-sycl-fp64-fallback.md)), while `iqa_filter_pixel()` sums its fp32 products in `double`; no host round trip inside a frame, one queue wait per frame; every CPU option keeps working (`enable_lcs`, `enable_db`, `clip_db` from [ADR-1365](1365-sycl-twin-cpu-option-parity.md)); one behaviour, one implementation (HISS-19).

## Decision

We will decimate on the device with the CPU's exact arithmetic and remove the host conversion:

1. **Raw samples up, conversion on the device.** `submit()` packs the luma rows `picture_copy()` reads (uint8, or uint16 at 10 / 12 / 16 bits) into pinned staging, uploads each plane in one DMA and launches `launch_decimate()`, which multiplies by `picture_copy()`'s divisor as an exact reciprocal. Scale 1 is the plain conversion.
2. **Exact decimation without fp64.** Each output is `iqa_filter_pixel()` at `(x * scale, y * scale)`: offsets `-scale/2 .. scale-1-scale/2`, `KBND_SYMMETRIC` edges, and the CPU's fp32 product `sample * (1.0f / (scale * scale))`. Every product is a multiple of 2^-52 and a window sums below 2^8.001, so the int64 sum in units of 2^-52 is exact, as is the CPU's `double` sum; one round-to-nearest-even conversion to fp32 then gives the CPU's `(float)sum` bit for bit. This holds up to scale 128; the context check refuses anything above. The plane size comes from `iqa_decimate_dim()`, now shared by `iqa/decimate.c` and the twin through the include-free `iqa/decimate_dim.h`.
3. **The gate refuses only what the device cannot do.** `check_context_sycl()` returns `-ENOTSUP` when the decimated plane is smaller than the 11x11 Gaussian or the scale exceeds 128, and 0 otherwise, so the twin serves every broadcast size. The CLI warning wording stays as it is: the context check returns a status, not a reason, and a reason channel would be a second mechanism next to ADR-1324's.
4. **Frame means rounded to fp32 as `iqa_ssim()` does.** `iqa_ssim()` returns each mean as `(float)(sum / (double)(w * h))`; the twin now rounds its `float_ssim` and `float_ssim_{l,c,s}` means the same way. Without it, a frame whose CPU mean rounds to exactly 1 (the first four frames of the BBB 4K pair) reported 93.6 dB under `enable_db` against the CPU's 121 dB `clip_db` ceiling. We accept that the default linear scores move by less than half an fp32 ulp of the mean (at most 6e-8).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Device decimation in int64 fixed point + raw upload (chosen) | Decimated planes bit-identical to the CPU on every depth and scale 1-128; 4x less PCIe traffic at 8 bits; no host pass | New kernel; int64 adds are emulated on Xe | — |
| fp32 accumulation of the window | Simplest kernel | Differs from the CPU's exact sum whenever the tap is not a power of two (scales 3, 5, 6, 7, 9, 10) | Not the CPU's arithmetic |
| Float-float (two-sum) accumulation | No int64 | Up to 100 terms can need more than 48 bits; the final rounding of `hi + lo` is not provably the CPU's | Harder to prove; no faster |
| Decimate on the host, upload the small planes | No new kernel | A full-frame host pass per plane every frame, on the thread that feeds the device | Violates device residency; the host pass is the work the twin exists to move |
| Keep scale 1 only and the CPU fallback (ADR-1324) | No change | `float_ssim` never runs on SYCL at 1080p or 4K | RC3 asked for the device path |
| Consume the shared graph frame instead of an own upload | One upload shared with VIF / ADM / motion | Changes the ADR-0458 self-contained submit/collect posture and the graph slot contract | Separate decision; noted as a follow-up |
| Keep the double frame mean | Default output bit-identical to before | `enable_db` differs from the CPU by up to 27 dB on near-identical frames | Breaks the ADR-1221 / ADR-1365 contract |

## Consequences

- **Positive**: `--backend sycl --feature float_ssim` and models run `float_ssim_sycl` at every size, 1080p and 4K included, with no fallback warning. The decimated planes match `iqa_decimate()` byte for byte on an Arc B580 and a UHD 770 (8 / 10 / 12 / 16 bits, every scale from 1 to 10, odd sizes; 50 planes per device). At scale 1 the output is byte-identical to the previous twin except the fp32 rounding of the mean. At 4K on an Arc B580 the twin takes 6.7 ms per frame (0.6 ms above the CLI's own per-frame floor) against 11.0 ms for the CPU extractor on 16 threads; the previous CPU fallback took 29.3 ms at the CLI's default thread count. Scale 1 at 4K drops from 10.6 to 7.8 ms on the B580 because the fp32 upload is gone.
- **Negative**: the remaining difference to the CPU is the twin's fp32 SSIM stage (combined Wang formula, Research-0985): up to 4.3e-5 on BBB 1080p and 4K at the auto scale, inside the 5e-5 cross-backend tolerance, but 7.8e-5 on BBB 1080p at an explicit `scale=1`, which predates this change. `enable_db` magnifies it by `4.34 / (1 - ssim)` (0.22 dB at 30 dB). The twin now fails `submit()` with an error instead of dereferencing NULL when `vmaf_read_pictures_sycl()` passes no host pictures; that device-buffer path still cannot run it.
- **Neutral / follow-ups**: the CUDA, HIP and Metal twins stay scale 1 only (RC3 rows in `docs/state.md` point at this design). A per-pixel L·C·S headline (the `enable_lcs` kernel's terms, clamped like `ssim_tools.c`) measured within 9e-7 of the CPU everywhere, but identical frames then need a correctly rounded `sqrt` to stay exactly 1; tracked as its own row. Guarded by `test_sycl_float_ssim_parity` (and `_large`), `test_feature_backend_twin`, `test_gpu_float_ssim_auto_scale_contract` and `test_vmaf_feature_backend_sycl`.

## References

- req: task brief (2026-09-29): "Implement scale>1 on the device (downsample kernel matching the CPU arithmetic order; then the existing SSIM path on the downsampled planes), including the auto-scale rule so `--feature float_ssim` at 4K runs on SYCL. Keep everything device-resident", quoting the maintainer: "there shouldnt be any gpu cpu rountrips".
- req: coordinator note (2026-09-29): "keep those options working at scale>1 and include them in your parity table".
- [Research-2130](../research/2130-sycl-float-ssim-device-decimation.md) — exactness proof, bit-identity harness, parity and timing tables.
- [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) — the context check; [ADR-1359](1359-cli-feature-backend-twin.md) — `--feature` twin routing; [ADR-1365](1365-sycl-twin-cpu-option-parity.md) — the options; [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md) — perfect-score dB contract; [ADR-0458](0458-sycl-cambi-ssim-slm-staging.md) — SLM staging; [ADR-1206](1206-gpu-parity-large-fixture-variants.md) — large-fixture variants.
