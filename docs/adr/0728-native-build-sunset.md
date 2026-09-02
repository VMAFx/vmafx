<!-- markdownlint-disable MD013 MD060 -->
# ADR-0728: Sunset Legacy Native Build Modes — Phase 4b.9 Follow-On

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `vmafx`, `breaking`

## Context

ADR-0691 (VMAFX Phase 1C — Drop Legacy Build Paths) and ADR-0710 (VMAFX CI
Slim-Down v2) decided to sunset several CI build configurations as part of the
container-first, production-Docker-image posture established by ADR-0686 and
ADR-0701. Both ADRs reached Accepted status before the implementing PR landed;
this PR is the mechanical execution of what those ADRs decided.

The build modes being dropped are:

| Mode | Where | Rationale |
|---|---|---|
| `Build — Windows MinGW64 (CPU)` | `libvmaf-build-matrix.yml` `windows:` job | MinGW64 is not a VMAFX production target; Windows coverage via MSVC (ADR-0691) |
| `Build — Ubuntu i686 gcc (CPU, no-asm)` | `libvmaf-build-matrix.yml` matrix entry | Fork is 64-bit only; ADR-0151 contract documented for historical record (ADR-0691) |
| `Build — Ubuntu gcc (CPU) + DNN` | `libvmaf-build-matrix.yml` matrix entry | Superseded by `build.yml` Linux full-build with `-Denable_dnn=enabled` (ADR-0710) |
| `Build — Ubuntu clang (CPU) + DNN` | `libvmaf-build-matrix.yml` matrix entry | Same; clang DNN redundant against gcc full-build (ADR-0710) |
| `Build — macOS clang (CPU) + DNN` | `libvmaf-build-matrix.yml` matrix entry | Superseded by `Build — macOS (Clang, CPU + Metal)` in build.yml (ADR-0710) |
| `Build — Ubuntu Vulkan (T5-1b runtime)` | `libvmaf-build-matrix.yml` matrix entry | Folded into Linux full-build (ADR-0710) |
| `Build — macOS Vulkan via MoltenVK (advisory)` | `libvmaf-build-matrix.yml` matrix entry | Too fragile; no required gate; dropped entirely (ADR-0710) |
| `Build — Ubuntu HIP (T7-10b runtime)` | `libvmaf-build-matrix.yml` matrix entry | Folded into Linux full-build (ADR-0710) |
| `Build — macOS Metal (T8-1 scaffold)` | `libvmaf-build-matrix.yml` matrix entry | Folded into macOS leg of build.yml (ADR-0710) |
| `Build — Ubuntu gcc Static (CPU)` | `libvmaf-build-matrix.yml` matrix entry | Static pkgconfig verified within Linux full-build (ADR-0710) |
| `Build — Ubuntu CUDA Static` | `libvmaf-build-matrix.yml` matrix entry | NVCC-static interaction covered by Linux full-build (ADR-0710) |
| `Build — Ubuntu SYCL` | `libvmaf-build-matrix.yml` matrix entry | Folded into Linux full-build (ADR-0710) |
| `Build — Ubuntu SYCL + CUDA` | `libvmaf-build-matrix.yml` matrix entry | Folded into Linux full-build (ADR-0710) |
| `Build — Windows MSVC + oneAPI SYCL (build only)` | `libvmaf-build-matrix.yml` `windows-gpu-build:` | SYCL folded into Linux full-build; MSVC+oneAPI edge case deprioritised (ADR-0710) |
| `Cppcheck (Whole Project)` | `required-aggregator.yml` | Clang-tidy superset; removed per ADR-0710 |
| `Sanitizers — ASan + UBSan + MSan (address/thread/undefined)` | `required-aggregator.yml` | Replaced by combined `Sanitizers — ASan + UBSan (PR gate)` from sanitizers.yml (ADR-0710) |
| `python/vmaf/` standalone wheel publishing | N/A | No wheel-publication job existed at audit time; no-op per ADR-0691 |
| `Static + DNN` combo | N/A | No such combo existed at audit time; no-op per ADR-0691 |

The `required-aggregator.yml` is updated to reflect the new canonical check names
from `build.yml` and `sanitizers.yml`.

The user authorized this work as handoff state for Phase 4b.9 with the direction:
"container-internal only. No external .so/.deb/.rpm publishing. Docker images +
Helm chart are the user-facing artifacts."

## Decision

Execute the mechanical changes described in ADR-0691 and ADR-0710:

1. Remove the `windows:` MinGW64 job from `libvmaf-build-matrix.yml`.
2. Remove the i686, DNN, Vulkan, MoltenVK, HIP, Metal, Static CPU, CUDA Static,
   SYCL, and SYCL+CUDA matrix entries from `libvmaf-build-matrix.yml`.
3. Remove the `backend: sycl` entry from the `windows-gpu-build:` matrix.
4. Update `required-aggregator.yml`: remove the 10 dropped check names; add the
   3 new build.yml names and the sanitizers.yml ASan+UBSan PR gate name.
5. Update documentation in `docs/development/build.md` and `docs/development/release.md`.
6. Add a deprecations row in `docs/development/deprecations.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep all legacy build modes | Zero breakage risk | Per-PR CI wall time stays high; maintenance cost accumulates; not aligned with container-first posture | Already decided against in ADR-0691 + ADR-0710 |
| Drop only one mode at a time | Smaller PRs | Multiple PRs required; state described in ADR-0691/0710 stays diverged from reality longer | Single coordinated removal is cleaner; all modes were decided in prior ADRs |
| Keep MinGW64 as an optional (non-required) lane | GCC Windows coverage without blocking PRs | Added noise without signal; MinGW accumulates platform-specific workarounds | Not a shipping target |

## Consequences

- **Positive**: Per-PR CI queue is shorter. Required-check list accurately reflects the
  configurations the fork ships. No MinGW-specific workarounds accumulate. The
  `required-aggregator.yml` no longer waits for 10 dropped jobs that will never fire.
- **Negative (breaking)**: The matrix shrinks — this is intentional and matches the
  container-first posture (ADR-0686, ADR-0701). `CHANGELOG.d/removed/native-build-sunset.md`
  marks this BREAKING.
- **Neutral**: `libvmaf-build-matrix.yml` remains in the repo; it still exercises the
  CPU-only + ARM + CUDA + SYCL Ubuntu/macOS base legs as a secondary matrix alongside
  the canonical `build.yml`. The Windows GPU build retains only the CUDA leg pending
  a full consolidation pass.

## References

- Parent decisions: ADR-0691 (drop legacy build paths), ADR-0710 (CI slim-down v2)
- Container-first posture: ADR-0686, ADR-0701
- i686 historical context: ADR-0151
- Required Checks Aggregator: ADR-0313
- `req` (paraphrased): user authorized Phase 4b.9 handoff — container-internal only;
  Docker images + Helm chart are the user-facing artifacts; drop MinGW64, i686,
  Static+DNN combo, and python/vmaf wheel publishing
