<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1078: ms_ssim option parity across HIP and SYCL backends

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `hip`, `sycl`, `cuda`, `ms_ssim`, `parity`

## Context

The CPU extractor `float_ms_ssim.c` exposes four user-visible options:
`enable_lcs`, `enable_db`, `clip_db`, and `enable_chroma`.  The CUDA twin
(`integer_ms_ssim_cuda.c`) gained `enable_lcs`, `enable_db`, and `clip_db`
but was missing `enable_chroma`.  Cross-backend audit (2026-06-06) found:

| Option         | CPU | CUDA | HIP | SYCL |
|----------------|-----|------|-----|------|
| `enable_lcs`   | yes | yes  | yes | **no** |
| `enable_db`    | yes | yes  | **no** | **no** |
| `clip_db`      | yes | yes  | **no** | **no** |
| `enable_chroma`| yes | no   | yes | yes  |

When `enable_db=true` is set against HIP or SYCL, the option is silently
ignored and the backend returns raw linear MS-SSIM instead of dB.  When
`enable_lcs=true` is set against SYCL, no per-scale L/C/S metrics are emitted.

Additionally, the CUDA twin computed a `score` variable for dB conversion but
then appended the unconverted `msssim` value instead — a latent bug that had
no effect at default settings (`enable_db=false`) but would produce wrong
output if `enable_db=true` were used.

## Decision

Add `enable_db` and `clip_db` to the HIP backend with the corresponding code
path in `collect_fex_hip`.  Add `enable_lcs`, `enable_db`, and `clip_db` to
the SYCL backend with the corresponding code path in `collect_fex_sycl`.
Fix the CUDA latent bug (append `score` not `msssim`).

All backends now append the dB-converted score when `enable_db=true`, which
matches the CPU reference in `float_ms_ssim.c:extract()`.  At default
settings (`enable_db=false`, `clip_db=false`, `enable_lcs=false`) all output
is numerically identical to the pre-fix state.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Defer until HIP/SYCL chroma paths land | Smaller diff | Users cannot use dB output on HIP/SYCL | Options are independent of chroma; no reason to defer |
| Accept HIP/SYCL parity gap permanently | No change | Silent misbehaviour on non-default configs | Violates cross-backend parity contract |

## Consequences

- **Positive**: all four backends now honour `enable_lcs`, `enable_db`, and
  `clip_db` consistently.  Fixes latent CUDA bug that would have emitted raw
  linear scores when `enable_db=true`.
- **Negative**: none at default settings; behaviour change only when
  non-default options are explicitly passed.
- **Neutral / follow-ups**: CUDA `enable_chroma` gap remains (CUDA is
  luma-only for MS-SSIM); that is a separate feature addition, not a parity
  bug.

## References

- Cross-backend audit, 2026-06-06 agent session.
- CPU reference: `core/src/feature/float_ms_ssim.c:extract()`.
- CUDA twin: `core/src/feature/cuda/integer_ms_ssim_cuda.c`.
- HIP twin: `core/src/feature/hip/integer_ms_ssim_hip.c`.
- SYCL twin: `core/src/feature/sycl/integer_ms_ssim_sycl.cpp`.
- ADR-0243 (`enable_lcs` pattern), ADR-0138/0139 (parity tolerances).
