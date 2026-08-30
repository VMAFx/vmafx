<!-- markdownlint-disable MD013 MD060 -->
# ADR-0594: Per-kernel `hip_cu_extra_flags` dispatch — disable FMA contraction for `ssimulacra2_blur` HIP HSACO

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: `hip`, `build`, `ssimulacra2`, `numerics`

## Context

The fork's HIP backend builds device kernels via `hipcc --genco`, the resulting
HSACO blobs are `xxd`-embedded into the libvmaf .so, and the host TUs load
them at runtime via `hipModuleLoadData`. Each kernel is registered as a
single entry in the `hip_kernel_sources` dict in `core/src/meson.build`,
and every entry was compiled with the same flat command line: `--genco`,
`hip_offload_arch_flags`, `hip_include_flags`, and a fixed `-I` set.

The CUDA twin has a `cuda_cu_extra_flags` dict that pushes per-kernel nvcc
flags. Two kernels currently use it: `float_adm_score` and
`ssimulacra2_blur`, both with `['-Xcompiler=-ffp-contract=off', '--fmad=false']`.
The reason is documented in the surrounding comment: the
recursive Gaussian IIR pole-tracking in `ssimulacra2_blur` and the angle-flag
/ cube reductions in `float_adm` rely on IEEE-754 add/mul ordering. If the
compiler is allowed to fuse `n2 * sum - d1 * prev` into an FMA, the
intermediate rounding shifts and the recursion drifts past places=2 vs the
CPU / Vulkan `precise` reference within a handful of pyramid levels.

The HIP port of `ssimulacra2_blur.hip` exists and produces real device
code, but the meson scaffolding had no mechanism to feed it a per-kernel
flag. `hipcc` (amdclang++-driven on ROCm 6) honours `-ffp-contract=off` on
the device side just as nvcc honours `--fmad=false`, but every HIP kernel
was being compiled with the default `fast` contraction. The
`ssimulacra2_blur` HSACO would therefore silently FMA-fuse the IIR step
even though the parallel CUDA kernel doesn't — divergence is invisible at
build time but bites the cross-backend parity gate (ADR-0214) the first
time someone enables `--backend hip` for ssimulacra2 on a discrete AMD
GPU.

## Decision

Mirror the CUDA dispatch pattern. Introduce a `hip_cu_extra_flags` dict
in `core/src/meson.build` keyed by the same logical kernel name used
in `hip_kernel_sources`, defaulting to an empty list per kernel via
`.get(name, [])`. The hipcc `custom_target` command line gains
`per_kernel_flags` between `hip_include_flags` and the `-I` block, so the
fall-through case (no entry) is byte-identical to the prior command line
and only the listed kernels pick up extra flags. The first and currently
only entry is `'ssimulacra2_blur' : ['-ffp-contract=off']`, matching the
CUDA twin's intent.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Push the flag globally onto every HIP HSACO build | One-line meson change | Penalises every other kernel (the CIEDE, motion, VIF, ADM int64 kernels do not have FMA-sensitive reductions and lose codegen quality for no parity gain) | Over-broad; matches what we deliberately rejected on the CUDA side |
| Add a `#pragma clang fp contract(off)` inside `ssimulacra2_blur.hip` | Keeps the contract local to the source | hipcc / amdclang++ pragma support is finicky across ROCm versions; the CUDA twin uses the command-line knob; consistency wins | Inconsistent with the established CUDA pattern; harder to discover when debugging cross-backend drift |
| Compile `ssimulacra2_blur.hip` outside the dict loop with a one-off `custom_target` | Most explicit | Duplicates the include path / offload-arch wiring; future per-kernel flag needs would each cost another copy-paste | Does not scale; the dict pattern is the right abstraction (the CUDA side already proved this) |

## Consequences

- **Positive**: `ssimulacra2` on the HIP backend matches the CPU
  reference within the places=2 tolerance required by ADR-0214, removing
  a silent FMA-driven parity gap. The dict mechanism is now in place so
  future kernels that need IEEE-strict semantics (e.g. when porting
  `float_adm` device code to HIP) only need a one-line dict entry.
- **Negative**: The blur HSACO compiles slightly slower and the
  generated code is marginally larger on AMD targets, same trade-off
  the CUDA twin already absorbs. No runtime perf change observed on
  the iGPU (gfx1036) verification host.
- **Neutral / follow-ups**: When `float_adm_score` is ported from the
  HIP scaffold to a real kernel, add the same entry to
  `hip_cu_extra_flags`. Documented in the dict comment.

## References

- ADR-0214 — GPU-parity CI gate (cross-backend tolerance).
- ADR-0254 — HIP kernel embedding pipeline (T7-10b, `enable_hipcc`).
- ADR-0468 — `float_adm` HIP scaffold (future consumer of the same dict).
- PR #999 — `integer_ssim` HIP extractor (real kernel, no FMA sensitivity).
- PR #1000 — `ssimulacra2` HIP port (the kernel this ADR's flag protects).
- PR #1013 — `integer_ms_ssim` HIP backend.
- Source: `req` — user direction "no stubs anywhere" and the brief's
  call-out that `ssimulacra2_blur` needs the HIP `-ffp-contract=off`
  equivalent of `--fmad=false`, mirroring the existing
  `cuda_cu_extra_flags` pattern.
