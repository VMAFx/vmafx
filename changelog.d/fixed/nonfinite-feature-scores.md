- Fail the frame when any of the twelve non-finite score paths found in the
  metric-engine sweep would otherwise publish a plausible value. VIF and ADM
  no longer turn NaN into their configured minimum, SSIM and MS-SSIM no longer
  turn it into `max_db`, SSIMULACRA2 no longer turns it into the perfect
  `100.0`, TransNet no longer reports “not a boundary”, and the aggregate VMAF
  piecewise mapping no longer reports `0.0`. VIF, ADM, SSIM and MS-SSIM now
  validate complete output sets before publication across CPU, CUDA, HIP, SYCL
  and Metal hosts; SSIMULACRA2's scalar and SIMD hosts share that failure
  contract too. ADM defines a finite flat per-scale `0/0` as the same perfect
  `1.0` used by its aggregate flat-frame result, and handles ADM/AIM
  denominators independently. Raw ADM reductions are checked before their
  precision floor, and hidden MS-SSIM L/C/S atoms are checked even when their
  debug output is disabled. The documented unclipped perfect-score infinity for
  SSIM/MS-SSIM remains unchanged. See ADR-1302.
- The Netflix golden gate is unchanged at `271 passed, 12 skipped`, and the
  touched clang-tidy warning allowances are unchanged. The generated HISS
  baseline tightens from 276 to 268 after the helper extractions remove eight
  infractions; no Netflix golden assertion changes.
