# ADR-0745: extern "C" wrapping for integer_ssim CUDA entry-point kernels

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `cuda`, `correctness`, `p0`

## Context

`core/src/feature/cuda/integer_ssim/integer_ssim_score.cu` is compiled by
`nvcc` as a C++ translation unit. Without `extern "C"`, the C++ compiler
mangles the names of all `__global__` functions. The host glue
(`core/src/feature/cuda/ssim_cuda.c`) uses `cuModuleGetFunction` to look
up the three entry points by their unmangled names
(`"integer_ssim_horiz_8bpc"`, `"integer_ssim_horiz_16bpc"`,
`"integer_ssim_vert_combine"`). The mismatch caused
`CUDA_ERROR_NOT_FOUND (500)` at runtime, making `--feature ssim
--backend cuda` completely non-functional. Surfaced by the PR #77 `ncu`
profile.

Every other `.cu` file in the same directory tree already has this wrapper.
The `integer_ssim_score.cu` file was the sole exception, introduced when the
real integer_ssim CUDA path was added in ADR-0564.

## Decision

Wrap the three `__global__` entry points (and the surrounding macros and
`__device__ static` array) in an `extern "C" { ... }` block in
`integer_ssim_score.cu`, consistent with the pattern used in
`ssim_score.cu` and all other CUDA kernel TUs in this directory.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `extern "C"` wrap (chosen) | Minimal blast radius; zero semantic change; consistent with every other kernel TU in the tree | Requires maintaining the invariant on future additions | Correct minimal fix |
| Rename kernels to mangled form on the host | No `.cu` change needed | Would require computing the mangled names per NVCC/CUDA version; brittle across toolchain upgrades; host code becomes unreadable | Unmaintainable |
| Compile with `-x c` (plain C) | Eliminates the mangling issue entirely | Prevents use of CUDA C++ intrinsics (`reinterpret_cast`, `__shfl_down_sync` with typed args) already used in the kernel body | Would require rewriting valid C++ to pure C |

## Consequences

- **Positive**: `--feature ssim --backend cuda` works for the first time
  since ADR-0564. The integer_ssim CUDA path is now reachable.
- **Negative**: None — the change is a one-line open and one-line close
  around existing code.
- **Neutral / follow-ups**: `core/src/feature/cuda/AGENTS.md` updated with
  an invariant: all `__global__` kernels looked up by
  `cuModuleGetFunction` must be in an `extern "C"` block. An audit grep
  confirmed `integer_ssim_score.cu` was the only offending file.

## References

- PR #77 ncu profile surfaced `CUDA_ERROR_NOT_FOUND (500)`.
- ADR-0564: introduced the `ssim_cuda.c` / `integer_ssim_score.cu` split.
- `core/src/feature/cuda/ssim_cuda.c:122–127` — the three `cuModuleGetFunction` call sites.
