<!-- markdownlint-disable MD013 MD060 -->
# Research-1367: One strict FP line for every SYCL feature TU — what it guarantees, what it changes, what it costs

- **Status**: Active
- **Workstream**: [ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md), [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md), [ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md), [ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md)
- **Last updated**: 2026-09-29

## Question

`T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29`: `-fp-model=precise` does not
stop icpx from contracting `a * b + c` into an FMA inside kernel lambdas, and
leaves fp32 `/` and `sqrt` approximate on the device. Only the SpEED and
ssimulacra2 TUs added `-ffp-contract=off` and rounded division in the source.
Should every SYCL feature TU get one strict line, including correctly rounded
division and square root, and what does that do to each twin's agreement with
`--backend cpu` and to its 4K cost on an Arc B580?

## Sources

- `core/src/meson.build` (SYCL feature line, `sycl_icpx_aot_base_args`,
  `sycl_dependency`), `core/src/feature/sycl/*.cpp`, the CPU extractors they
  mirror.
- icpx 2026.1.1 (`vmaf-dev-mcp:ocloc`), Arc B580 (`bmg-g21`,
  `level_zero:0`) and UHD 770 (`adl-s`, `level_zero:1`) through WSL2 Level
  Zero, i9-12900K. Build: `CC=icx CXX=icpx`, `-Dsycl_icpx_aot_targets=bmg-g21,adl-s`,
  release, no LTO. Base: `perf/sycl-ssimulacra2-msssim-device-resident`
  (`a6afca951`); after: the same plus this change.
- Fixtures: the Netflix `src01_hrc00/01_576x324` pair (48 frames) and the
  first 22 frames of BBB 3840x2160 (`ref/dis_3840x2160_200f.yuv`).

## Findings

1. **Device arithmetic per flag set** (a stand-alone kernel computing
   `a * b + c` as one expression, `a / b` and `sycl::sqrt(fabs(a))` on
   4 194 304 random fp32 operands with exponents in [-20, 20], against the
   host evaluated in fp64 and rounded once; identical on both GPUs):

   | Compile line (link: plain `-fsycl`) | `a * b + c` | `/` | `sqrt` |
   |---|---|---|---|
   | `-fp-model=precise` | 507 408 differ | 1 219 433 | 343 307 |
   | + `-ffp-contract=off` | 0 | 1 219 433 | 343 307 |
   | + `-foffload-fp32-prec-div -foffload-fp32-prec-sqrt` | 0 | 0 | 0 |
   | same pair on the link only, not the compile line | 0 | 1 219 433 | 343 307 |

   With `-fno-sycl-rdc` (ADR-1360) the driver runs ocloc for each
   `spir64_gen` image at compile time and passes it
   `-ze-fp32-correctly-rounded-divide-sqrt` when the pair is on the compile
   line. ADR-1358's "the flags act only on the final image link" predates
   that change. `-fp-model=precise` removes `-fno-offload-fp32-prec-div/-sqrt`
   from the device front end but does not add the backend option; only the
   explicit pair does.

2. **The SPIR-V JIT image is still made at the link.** Under `-fno-sycl-rdc`
   the `spir64` part of a TU is stored as bitcode and device-linked by the
   final `-fsycl` link. A `-fsycl-targets=spir64` build with the pair on the
   compile line only keeps the approximate division (same counts as row 2);
   with the pair on the link the runtime builds the program with
   `-ze-fp32-correctly-rounded-divide-sqrt` (seen in `SYCL_UR_TRACE=2`) and
   all three counts are 0. The SYCL parity CI lane builds this way
   (`-Dsycl_icpx_aot_targets=`). On an AOT build run on a device outside the
   list (images for `dg2-g10` only, run on the B580 and the UHD 770) the
   runtime created the program from the AOT binary and the driver rebuilt it
   from its embedded SPIR-V with the recorded options: 0 mismatches with the
   pair on the compile line, the precise-only counts without it.

3. **The pair costs the link nothing.** `icpx -fsycl` plus the pair, or
   plus the whole strict line, links without a diagnostic and with the same
   `-l` / `-L` list (`-###` diff empty); it adds no device target, so the
   ADR-1360 concern about link-time AOT does not apply. On Windows the link is
   `link.exe`, which reports `LNK4044: unrecognized option '/fsycl'`; since
   ADR-1364 an MSVC build generates every image in one explicit
   `icpx -fsycl -fsycl-link` step, and that step takes the whole strict line
   (not measured on a Windows GPU).

4. **The ordering note.** `-fp-model=precise -ffp-contract=off` makes icpx
   print `-Woverriding-option` for every compile (host plus each device
   pass): 12 instances before (the SpEED TUs), 60 after, plus 43 from the icx
   x86 strict libraries that already use the same order. It is the intended
   override; putting contraction-off first is silently undone by precise
   (`T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07`).

5. **Per-twin parity**, maximum and mean absolute difference against
   `--backend cpu --precision max` over every output and frame, base -> after,
   Arc B580 (the UHD 770 is identical except where noted):

   | Twin | 576x324 max | 576x324 mean | 3840x2160 max | 3840x2160 mean |
   |---|---|---|---|---|
   | vif | 3.87e-07 -> 3.49e-07 | 6.29e-08 -> 5.42e-08 | 1.22e-07 -> 1.49e-07 | 2.79e-08 -> 2.68e-08 |
   | adm | 2.27e-07 -> 2.27e-07 | 9.15e-08 -> 9.15e-08 | 1.34e-06 -> 1.34e-06 | 1.01e-07 -> 1.01e-07 |
   | motion | 1.26e-05 -> 1.26e-05 | 4.58e-06 -> 4.58e-06 | 5.56e-06 -> 5.56e-06 | 7.92e-07 -> 7.92e-07 |
   | motion_v2 | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
   | float_psnr | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
   | float_motion | 3.05e-06 -> 3.09e-06 | 9.09e-07 -> 9.20e-07 | 2.67e-05 -> 2.29e-05 | 2.58e-06 -> 3.03e-06 |
   | float_vif | 2.71e-05 -> 3.81e-05 | 3.08e-06 -> 3.18e-06 | 3.18e-06 -> 3.14e-06 | 8.23e-07 -> 7.71e-07 |
   | psnr | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
   | float_moment | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
   | ciede | 1.18e-05 -> 1.14e-05 | 1.08e-05 -> 1.02e-05 | 9.71e-05 -> 4.53e-05 (UHD 770 4.54e-05) | 2.37e-05 -> 1.94e-05 |
   | float_ssim | 3.10e-07 -> 1.47e-07 | 1.88e-07 -> 2.60e-08 | 8.31e-05 -> 8.80e-05 (`scale=1`) | 4.69e-05 -> 4.89e-05 |
   | ssim | 1.40e-08 -> 7.40e-09 (UHD 770 1.47e-08 -> 7.15e-09) | 7.15e-09 -> 2.55e-09 | 7.76e-08 -> 5.59e-08 (UHD 770 5.57e-08) | 1.60e-08 -> 2.61e-08 |
   | float_ms_ssim | 6.96e-08 -> 6.92e-08 | 2.56e-08 -> 2.06e-08 | 2.84e-07 -> 4.69e-07 | 1.36e-07 -> 6.75e-08 |
   | psnr_hvs | 8.37e-05 -> 8.37e-05 | 3.60e-05 -> 3.60e-05 | 8.42e-04 -> 8.42e-04 | 2.41e-04 -> 2.41e-04 |
   | ssimulacra2 | 1.12e-12 -> 1.12e-12 | 5.10e-13 -> 5.10e-13 | 6.65e-12 -> 6.65e-12 | 1.10e-12 -> 1.10e-12 |
   | float_adm | 2.50e-05 -> 2.53e-06 | 1.68e-07 -> 4.62e-08 | 1.12e-06 -> 1.97e-07 | 1.36e-07 -> 2.74e-08 |
   | cambi | 0 -> 0 | 0 -> 0 | 2.22e-15 -> 2.22e-15 | 1.72e-16 -> 1.72e-16 |
   | speed_chroma | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
   | speed_temporal | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0 -> 0 |
   | default model (`vmaf_v1.0.16_3d0h`) features | 1.26e-05 -> 1.26e-05 | 8.86e-07 -> 8.86e-07 | 5.56e-06 -> 5.56e-06 | 1.19e-07 -> 1.19e-07 |

   Output byte-identical before and after: `adm`, `motion`, `motion_v2`,
   `float_psnr`, `psnr`, `float_moment`, `ssimulacra2`, `cambi`, both SpEED
   twins and the default model. `psnr_hvs` changes by at most 1.3e-6 dB (39
   of 192 values at 576x324), below the precision of the table. `float_ssim` at 3840x2160 is measured with `scale=1` on both sides:
   with the default automatic scale the twin declines 4K and the CPU extractor
   runs. Its 8e-5 drift there is the combined SSIM formula of the SYCL twin
   (`T-SYCL-ARC-FLOAT-SSIM-PARITY-2026-06-03`), before and after.

6. **Where parity got worse, the flagged operations are not the cause.**
   `float_vif` has a systematic offset at scale 0 (median 3.44e-6 before,
   3.62e-6 after, every frame), most likely from `sycl::log2` against the
   host's `log2f` and from the CPU evaluating `1 + x / y` in fp64
   (`vif_sigma_nsq` is a `double`); not isolated. Its scale-3 maximum is one
   frame. `float_ms_ssim` and `ssim` at
   4K move max and mean in opposite directions: the CPU computes luminance,
   contrast and structure partly in fp64 and sums in its own order. In each
   case the change makes the operations the flags control match the CPU and
   reshuffles which frame carries the residual.

7. **`integer_vif_sycl` depended on contraction.** The CPU computes
   `sv_sq = sigma2_sq - g * sigma12` in fp64 and truncates it
   (`integer_vif.c`); the fp32-only twin (ADR-0220) got a single rounding only
   because icpx fused it. With contraction off and the expression as written,
   the mean difference on the Netflix pair went from 6.29e-8 to 3.75e-7 (max
   3.87e-7 -> 1.00e-6; measured on a master-based build, `vif_sycl` is the
   same file there). Written as `sycl::fma(-g, sigma12, sigma2_sq)` it is
   5.42e-8 (max 3.49e-7): the one intended fusion, plus the correctly rounded
   division of `g`.

8. **Cost.** No twin is measurably slower; see §Cost.

9. **The ADR-0214 gate.** `scripts/ci/cross_backend_parity_gate.py --backends
   cpu sycl` on the Netflix pair, one feature per run, on each GPU: all 15
   runnable cells `OK` before and after (at the gate's `%.6f` output:
   `float_adm` 2.5e-5 -> 2.0e-6, `float_vif` 2.8e-5 -> 3.8e-5 against 5e-5,
   the rest unchanged). The `cambi` and `motion` cells abort with `KeyError`
   on stale metric names in the gate itself
   (`T-CI-PARITY-GATE-STALE-METRIC-KEYS-2026-09-29`); both twins are compared
   above instead.

## Cost

Arc B580, 3840x2160, base -> after, milliseconds per frame. Every GPU run holds
`/f/gpu.lock`; seven other agents were building on the same host.

**The CLI method** (`(t(22) - t(2)) / 20`, median of 3, `--precision max`
JSON output) is too noisy here to resolve 10%: `adm` and `motion_v2`, whose
kernels and output did not change, read +50% and -43%.

| Twin | before | after | | Twin | before | after |
|---|---|---|---|---|---|---|
| vif | 6.55 | 5.39 | | ciede | 7.89 | 7.90 |
| adm | 4.63 | 6.93 | | float_ssim (`scale=1`) | 9.95 | 11.28 |
| motion | 8.06 | 5.60 | | ssim | 8.14 | 9.72 |
| motion_v2 | 7.07 | 4.05 | | float_ms_ssim | 12.93 | 14.98 |
| float_psnr | 9.44 | 7.01 | | psnr_hvs | 21.39 | 20.45 |
| float_motion | 6.47 | 4.56 | | ssimulacra2 | 34.52 | 31.95 |
| float_vif | 8.72 | 8.43 | | float_adm | 7.97 | 8.50 |
| psnr | 6.80 | 6.51 | | cambi | 8.17 | 8.24 |
| float_moment | 6.64 | 6.10 | | speed_chroma | 7.54 | 8.10 |
| default model | 48.76 | 45.57 | | speed_temporal | 9.35 | 8.78 |

Over 100 frames and 5 repetitions the three SSIM twins that looked slower
read `ssim` 9.92 -> 10.05, `float_ssim` 13.55 -> 13.40 and `float_ms_ssim`
15.14 -> 15.74.

**In memory.** A harness links each build's `libvmaf.so`, loads 8 frames of
the pair once, then feeds 50 frames through `vmaf_read_pictures()` with one
twin registered (`vmaf_use_feature(<twin>_sycl)`) and times from the first
fed frame to the end of the flush; median of 5 runs, base and after
interleaved (7 for the last three rows of the right column):

| Twin | before | after | change | | Twin | before | after | change |
|---|---|---|---|---|---|---|---|---|
| vif | 6.28 | 6.42 | +2.1% | | float_ssim (`scale=1`) | 12.07 | 11.70 | -3.1% |
| adm | 6.42 | 6.25 | -2.7% | | ssim | 7.47 | 6.94 | -7.0% |
| motion | 6.11 | 5.92 | -3.1% | | float_ms_ssim | 15.46 | 15.54 | +0.5% |
| motion_v2 | 6.64 | 6.70 | +0.9% | | ssimulacra2 | 34.01 | 34.05 | +0.1% |
| float_psnr | 7.51 | 7.73 | +2.9% | | float_adm | 9.61 | 8.83 | -8.1% |
| float_motion | 7.33 | 7.31 | -0.3% | | cambi | 8.17 | 7.97 | -2.4% |
| float_vif | 8.19 | 7.98 | -2.6% | | speed_chroma | 7.13 | 6.59 | -7.6% |
| psnr | 6.90 | 7.01 | +1.7% | | psnr_hvs | 20.82 | 21.47 | +3.1% |
| ciede | 8.56 | 8.55 | -0.0% | | float_moment | 5.71 | 5.61 | -1.8% |
| | | | | | speed_temporal | 6.91 | 7.00 | +1.3% |

Unchanged kernels move by up to 8% between runs (`float_moment` read +7.0%
over 5 runs and -1.8% over 7). The largest increase is `psnr_hvs`, +3.1%
(+6.9% over the first 5 runs). No twin is more than 10% slower, so none is
exempt from the line. At 4K the per-frame cost is dominated by the picture
copies and the upload; the extra instructions of a correctly rounded division
and of an unfused multiply-add do not show.

## Alternatives explored

- **Contraction off only.** Leaves `/` and `sqrt` approximate (finding 1),
  so every twin that divides keeps that difference; not measured per twin.
- **Pair on the link only.** Leaves every AOT image approximate (finding 1,
  last row).
- **`div_rn` / `sqrt_rn` in every kernel.** Exact and fast, but every `/` in
  20 TUs is rewritten and each new one must be remembered; the compiler flag
  gives the same result.

## Open questions

- The Arc A380 (`dg2-g11`, no fp64) was measured on 2026-09-30 under the xe
  kernel driver (NEO 26.35.39758.10, IGC 2.41.5). `test_sycl_fp_arith_contract`
  passes on the SPIR-V JIT image and on a `dg2-g11` AOT image. `--suite sycl`
  fails the same 16 tests on master `10f27efe2` (JIT) and on this branch (JIT
  and AOT). Each of the 16 runs a kernel that uses scratch memory, and on that
  driver standalone kernels that use private memory or spill registers return
  wrong values on every work-item, so the failures say nothing about this
  change. A run under the i915 driver is still open.
- `div_rn` / `sqrt_rn` (ADR-1358 / ADR-1363) now duplicate what the line
  gives on icpx; whether plain `/` is as fast in the SpEED eigenvalue kernel
  is unmeasured.
- CUDA and HIP still contract in all but two kernels each
  (`T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`, `T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`).

## Related

- ADR-1367, ADR-1358, ADR-1363, ADR-1360, ADR-0202, ADR-0214, ADR-0220.
- `docs/state.md`: `T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29` (closed),
  `T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`, `T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`,
  `T-CLI-FLOAT-MOMENT-NO-TWIN-2026-09-29`.
