<!-- markdownlint-disable MD013 MD060 -->
# ADR-0759: HIP ADM — AdmBufferHip passed by pointer (F3 fix)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: `hip`, `performance`, `cuda`, `kernel`, `adm`, `fork-local`

## Context

PR #95 (HIP backend audit, Research-0755) identified a P1 finding (F3):
`AdmBufferHip` (~272 bytes, containing 6 DWT band sub-structs each with 4 device
pointers, plus 8 additional device-pointer fields) was passed **by value** in the
`__global__` kernel signatures in `integer_adm/adm_csf.hip` (two kernels via
macro expansion) and `integer_adm/adm_cm.hip` (two kernels directly). This mirrors
the F3 finding on the CUDA side (PR #93 audit), where the same pattern in the CUDA
twins was also flagged.

Passing a 272-byte struct by value to a HIP/ROCm kernel has two concrete costs:

1. The HIP runtime must marshal the full struct through the kernel argument buffer
   on every kernel launch. ROCm targets (gfx906/gfx90a/gfx10/gfx11) have per-launch
   argument buffer limits that vary by target; a 272-byte struct consumes a large
   fraction of this budget.
2. Each GPU thread (or group of threads) receives a copy of the struct preamble during
   kernel setup, increasing register pressure and per-launch overhead.

The fix is a pointer-passing convention: the host allocates a device-side copy of
`AdmBufferHip` once at init time (`hipMalloc` + `hipMemcpy`), and the kernel
signatures receive `const AdmBufferHip * __restrict__ buf_ptr` instead. Because
the struct contains only device pointers that are stable across the extractor's
lifetime (they are set up in `init_fex_hip` and never change), a single init-time
copy suffices — no per-frame update is required.

## Decision

Replace `AdmBufferHip buf` (by value) in all four affected kernel signatures with
`const AdmBufferHip * __restrict__ buf_ptr`. Allocate a device-side copy of the
struct in `init_fex_hip` via `hipMalloc` + `hipMemcpy(hipMemcpyHostToDevice)`.
Store the device pointer as `AdmStateHip::buf_dev`. Free it in `close_fex_hip`.
Pass `&buf_dev` (address of the device pointer variable) through the host
`void *args[]` array to `hipModuleLaunchKernel`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-field extraction via raw ldg pointer | No struct on device | 272-byte struct has too many fields (6 nested sub-structs, 4+8 device ptrs each); kernel bodies would need 20+ explicit pointer params per launch | Too many parameters; error-prone |
| `__constant__` memory | Zero-cost device access (broadcast through constant cache) | Requires `hipMemcpyToSymbol`; `AdmBufferHip` contains device pointers that may differ per-context instance (multi-GPU not scoped but pointer values differ); `__constant__` is static (global, not per-instance) | Not per-instance safe; deferred to follow-up |
| Host-side struct-by-pointer via device allocation (CHOSEN) | One allocation per extractor lifetime; zero per-frame overhead; matches CUDA F3 fix pattern | Extra 272-byte device allocation per init | Best balance of simplicity and correctness |
| Unmodified (keep by-value) | No change required | Per-launch arg-buffer overhead; argument limit risk on older GCN targets | Fails the ADR-0214 correctness/performance bar |

## Consequences

- **Positive**: Eliminates ~272 bytes of per-launch argument marshalling overhead on all
  four ADM kernels (adm_csf_kernel_1_4, i4_adm_csf_kernel_1_4, i4_adm_cm_line_kernel,
  adm_cm_line_kernel_8). Reduces risk of hitting kernel-argument buffer limits on
  older GCN targets.
- **Positive**: Matches the established CUDA F3 fix pattern (PR #93 / PR #96), keeping
  HIP and CUDA host launch code structurally symmetric.
- **Negative**: One additional `hipMalloc` (272 bytes) per extractor init. Negligible.
- **Neutral / follow-ups**: `__constant__` memory promotion (no per-context pointer
  issue once multi-GPU scope is confirmed) is deferred. `AdmFixedParametersHip` (~244 bytes,
  also by-value) is a follow-up candidate but is out-of-scope for this PR. The
  AGENTS.md invariant note added by this PR covers future ADM kernel additions.

## References

- PR #95 (Research-0755 HIP backend audit, F3 finding)
- PR #93 (CUDA F3 equivalent fix — CUDA twin's by-value struct issue)
- PR #96 (CUDA post-audit pass — confirmed F3 CUDA fix)
- `core/src/feature/hip/AGENTS.md` — prior invariant notes on pointer-passing conventions
- ADR-0539 (`adm_cm.hip` original port)
- ADR-0214 (GPU parity gate, places=4)
- `req`: "Fix the F3-equivalent finding from PR #95's HIP audit: AdmBufferHip (~272 bytes) passed BY VALUE in HIP kernel signatures"
