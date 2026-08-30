<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1071: Promote HIP ms_ssim_vert_lcs to double precision (ADR-0990 parity)

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `hip`, `precision`, `ms_ssim`, `cross-backend-parity`

## Context

ADR-0990 (2026-06-03) promoted the CUDA `ms_ssim_vert_lcs` kernel's per-pixel
L/C/S computation, warp/block reductions, and per-block partial buffers from
`float` to `double`, closing a ~0.004 drift at scale 0 relative to the CPU
reference (`ssim_tools.c` uses `2.0 *` double literals). The host struct fields
`c1`, `c2`, `c3` and the pinned partial arrays were also promoted to `double`.

The HIP twin (`core/src/feature/hip/integer_ms_ssim/ms_ssim_score.hip` and
`integer_ms_ssim_hip.c`) did not receive the same fix. A cross-backend audit on
2026-06-06 (moment, ciede2000, ms_ssim extractors) confirmed:

1. `ms_ssim_vert_lcs` kernel takes `float c1, c2, c3` and writes `float *l_partials`,
   `float *c_partials`, `float *s_partials` — same pre-ADR-0990 precision posture.
2. `MsSsimStateHip` has `float c1, c2, c3`, `float *h_{l,c,s}_partials[5]`, and
   device partial allocations sized `sizeof(float)` — mismatched with CUDA's `double`.
3. Options `enable_db` and `clip_db`, exposed by both the CPU and CUDA extractors,
   were absent from the HIP extractor's `options[]` array, causing silent score drift
   when callers request dB output.

## Decision

Apply the ADR-0990 precision fix to the HIP backend:

- Kernel (`ms_ssim_score.hip`): promote `c1/c2/c3` parameters from `float` to
  `double`; promote per-pixel `my_l/my_c/my_s` from `float` to `double`; use
  double literals (`2.0 *`) for the L/C/S numerators; promote shared reduction
  arrays to `double`; use `__shfl_down` on `double` operands; write `double *`
  output partials.
- Host (`integer_ms_ssim_hip.c`): promote `MsSsimStateHip.c1/c2/c3` to `double`;
  compute them using `const double L = 255.0`; resize device and pinned-host
  partial allocations to `sizeof(double)`; resize `hipMemcpyAsync` DtoH copies
  accordingly; promote `h_{l,c,s}_partials` arrays to `double *`.
- Add `enable_db` and `clip_db` to the HIP extractor's `options[]` and apply
  the dB conversion in `collect_fex_hip()`, matching the CPU and CUDA paths.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `float` partials in HIP | No kernel change required | Persistent ~0.004 per-scale drift; fails ADR-0214 places=4 gate on AMD hardware | Correctness non-negotiable |
| Apply only to host, not kernel | Simpler diff | Kernel still writes `float`; host double allocation would be written by `float` kernel causing UB (type-punning) | Wrong: kernel and host must agree on element size |
| Annotate as known divergence | Zero code change | Requires weakening ADR-0214 gate for HIP; unacceptable | Violates correctness-first principle |

## Consequences

- **Positive**: HIP ms_ssim L/C/S precision matches CUDA and CPU within ADR-0214
  places=4; `enable_db`/`clip_db` options now functional on HIP.
- **Negative**: `double` partial buffers are 2× larger in device VRAM (typically
  <100 KB total for 5 scales at 1080p; negligible). GCN/RDNA has native fp64
  support, so no emulation penalty.
- **Neutral / follow-ups**: SYCL ms_ssim uses `float` partials intentionally due
  to Intel Arc A380 lacking native fp64; no change needed there (documented as
  places=3 tolerance in ADR-0214 SYCL exception).

## References

- ADR-0990 (CUDA ADR-0139/ADR-0990 double-precision fix — the root fix being ported here)
- ADR-0139 (AVX2/AVX-512 double-precision fix)
- ADR-0214 (cross-backend places=4 CPU-parity gate)
- ADR-0285 (HIP ms_ssim extractor scaffolding)
- Cross-backend audit 2026-06-06: moment, ciede2000, ms_ssim
