<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1223: The CUDA backend requires compute capability 8.0 (Ampere), and CI standardises on CUDA 13.3.1

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `build`, `ci`, `docs`

## Context

The CUDA backend shipped cubins from Turing (`sm_75`) through Blackwell
(`sm_120`), plus a `compute_80` PTX as the backward-JIT floor and — on CUDA
12.x only — a `compute_50` PTX. That coverage was set by
[ADR-0122](0122-cuda-gencode-coverage-and-init-hardening.md) when the fork
still aimed at "every currently-shipping consumer generation".

Two things have changed.

**The project's target has been stated.** The maintainer's direction is that
this fork does not carry old hardware. Turing shipped in 2018; CUDA 13.x had
already dropped Maxwell, Pascal and Volta, so `sm_75` was the last
pre-Ampere architecture still receiving a cubin.

**The CI pins had drifted and one was stale.** `build.yml`'s Linux leg
installs CUDA **13.3.1** via `Jimver/cuda-toolkit` v0.2.36. Its Windows leg,
and both legs of `libvmaf-build-matrix.yml`, were still on **13.2.0**. The
matrix carried a comment pinning it there because "the CI installer
(Jimver/cuda-toolkit) does NOT yet publish a 13.3.0 build
(T-CI-JIMVER-CUDA-133-NOT-AVAILABLE)". That was true of v0.2.35; it is stale
at v0.2.36, which the Linux leg already uses to install 13.3.1 successfully.
The Windows legs never used Jimver at all — they fetch NVIDIA's network
installer directly, so they were never blocked by it. Both
`cuda_13.3.0_windows_network.exe` and `cuda_13.3.1_windows_network.exe`
resolve (HTTP 200); `13.3.2` does not exist.

Separately, nothing checked the device's compute capability. Dropping the
`sm_75` cubin without a guard would surface as `CUDA_ERROR_NO_BINARY_FOR_GPU`
(222) from `cuModuleLoadData`, inside whichever feature extractor happened to
load first — an opaque failure a long way from its cause.

## Decision

We will raise the CUDA architecture floor to **compute capability 8.0
(Ampere)**: drop the `sm_75` gencode entry and the CUDA-12-only `compute_50`
PTX, and move the `clang`-CUDA fallback path from `--cuda-gpu-arch=sm_75` to
`sm_80`. `vmaf_cuda_state_init()` gains an explicit capability check that
returns `-ENOTSUP` with a message naming the device, its capability, the
required floor and the CPU-backend escape hatch — checked before a primary
context is retained, and also on the caller-supplied-context path.

Every CI lane standardises on **CUDA 13.3.1**, matching `build.yml`'s Linux
leg and the dev container.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Floor at 8.0, guard at init, CI on 13.3.1 (chosen) | Matches the project's stated hardware target; one toolkit version everywhere; unsupported GPUs fail with a clear message | Turing users must use the CPU backend or an older release | — |
| Keep `sm_75` | No user loses hardware support | Carries a 2018 architecture the project has declared out of scope, and a seventh cubin in every fatbin | Contradicts the maintainer's direction |
| Drop `sm_75` without the runtime guard | Smallest diff | A Turing user gets `CUDA_ERROR_NO_BINARY_FOR_GPU` from a feature extractor, with nothing pointing at the cause | Turns a supported-hardware decision into a debugging exercise for the user |
| Floor at 9.0 (Hopper) | Even fewer cubins | Would drop the RTX 30xx/40xx consumer line, including this project's own RTX 4090 dev GPU | Not the stated target |
| Keep CI on 13.2.0 | No CI churn | Leaves the toolkit split that the (now stale) Jimver comment described, and diverges from the container and from `build.yml` | The blocker no longer exists |

## Consequences

- **Positive**: one CUDA version across CI, container and workstation. Six
  cubins instead of seven, so a smaller fatbin. An unsupported GPU produces
  one actionable error at init rather than a load failure later.
- **Negative**: Turing (RTX 20xx, GTX 16xx, Tesla T4) and older can no longer
  use the CUDA backend. They still work on the CPU backend, and on any release
  up to and including v3.2.1.
- **Neutral / follow-ups**: `compute capability 8.0` is also the floor CUDA
  Tile C++ requires, so this removes one precondition for a possible later
  adoption — but that is a separate decision, and the audit of whether Tile
  suits this codebase at all is not what motivated this change.
  `scripts/ci/gpu_ulp_calibration.yaml` keeps its Turing placeholder row,
  marked as retired rather than deleted, so the historical ULP shape is not
  lost.

## Verification

- `cuobjdump --list-elf` on a built fatbin reports exactly
  `sm_80 sm_86 sm_89 sm_90 sm_100 sm_120`, and `--list-ptx` reports
  `sm_80` and `sm_120` — no `sm_75`, no `compute_50`.
- End-to-end on the RTX 4090 (`sm_89`), Netflix checkerboard golden pair
  (`checkerboard_1920_1080_10_3_0_0` vs `..._1_0`, 1920x1080 8-bit):
  CUDA `45.315104`, CPU `45.315104` — identical, so the change is
  behaviour-neutral on supported hardware.
- `core/test/test_cuda_arch_floor.c` pins the predicate without a GPU, which
  matters because the architectures it most needs to cover (Turing and older)
  are exactly the ones no runner in the fleet has.

## References

- [ADR-0122](0122-cuda-gencode-coverage-and-init-hardening.md) — the gencode
  coverage this supersedes in part.
- [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) — why the floor is
  at least CUDA 13.2 on Ubuntu 26.04.
- `T-CI-JIMVER-CUDA-133-NOT-AVAILABLE-2026-06-06` in
  [`docs/state.md`](../state.md) — the closed ticket whose stale comment kept
  the build matrix on 13.2.0.
- [NVIDIA CUDA Tile C++ requirements](https://developer.nvidia.com/blog/develop-high-performance-gpu-kernels-in-cpp-with-nvidia-cuda-tile/)
  — compute capability 8.0+, noted only as context for the follow-up.
