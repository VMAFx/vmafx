<!-- markdownlint-disable MD013 MD060 -->
# ADR-0419: Gate SVE2 build probe to non-Darwin hosts

- **Status**: Accepted
- **Date**: 2026-05-11
- **Deciders**: lusoris, lawrence
- **Tags**: `build`, `simd`, `macos`, `arm64`

## Context

The aarch64 SVE2 path in [core/src/feature/arm64/ssimulacra2_sve2.c](../../core/src/feature/arm64/ssimulacra2_sve2.c) (per [ADR-0213](0213-ssimulacra2-sve2.md), T7-38) is built when [`core/src/meson.build`](../../core/src/meson.build) `cc.compiles()` accepts `-march=armv9-a+sve2` against a probe TU that includes `<arm_sve.h>`. The runtime dispatcher in [`core/src/arm/cpu.c`](../../core/src/arm/cpu.c) reads `getauxval(AT_HWCAP2) & HWCAP2_SVE2` to decide whether to swap NEON fn-pointers for SVE2 ones — and that probe is `__linux__`-gated, because `getauxval` is Linux-specific.

A fork contributor reported a local macOS-arm64 build failure with a wall of SVE-related compile errors. Recent Apple Clang ships `<arm_sve.h>` and accepts `-march=armv9-a+sve2` (so the meson probe returns true), but the SVE2 SSIMULACRA 2 TU itself fails to compile under Apple Clang's incomplete intrinsics surface. Even if it built, the resulting object code would be unreachable: every Apple Silicon part to date (M1–M4) implements ARMv8.x without SVE2, and the runtime probe never fires on Darwin to begin with. Building it is pure cost — compile failures on contributor machines, and dead text in the binary on the GHA `macos-latest` lane that does happen to build.

## Decision

We will short-circuit `is_sve2_supported = false` when `host_machine.system() == 'darwin'` in [`core/src/meson.build`](../../core/src/meson.build), so the `cc.compiles()` SVE2 probe never runs on Darwin and the SVE2 static library is never registered for macOS targets. The probe still runs on Linux (the only host where the runtime detection can fire) and other non-Darwin platforms behave identically to before.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Darwin meson gate (chosen) | One-line surgical fix; matches the runtime `__linux__` gate; no rebase impact on `arm64_v8` core | Adds a per-OS branch in the probe | Smallest blast radius — runtime path is already Linux-only, so the build-time gate just removes the inconsistency |
| Drop the SVE2 path entirely | Removes complexity | Loses real perf on Linux ARMv9 servers (Graviton 4, Ampere AmpereOne); reverts [ADR-0213](0213-ssimulacra2-sve2.md) | SVE2 is a real perf win on the platforms that have it; the bug is platform-specific |
| Patch Apple Clang's SVE2 surface in our TU with `#ifdef __APPLE__` shims | Keeps the cross-platform probe symmetric | Maintenance burden, dead code on hardware that doesn't exist, ongoing Apple Clang version drift | We'd be carrying a workaround for a path that can never execute |
| Wait for upstream Netflix to gate it | No fork-local change | Upstream doesn't have SVE2 dispatch; this is fork-local code | Nothing to wait for |

## Consequences

- **Positive**: macOS-arm64 builds (both Apple Silicon GHA runners and contributor laptops) no longer attempt to compile the SVE2 TU. Matches the runtime detection's `__linux__` gate.
- **Negative**: None — the SVE2 path on Darwin was already unreachable at runtime.
- **Neutral / follow-ups**: If a future Apple Silicon part ships SVE2 (no public roadmap), reverse this gate and add the corresponding Darwin runtime probe in [`core/src/arm/cpu.c`](../../core/src/arm/cpu.c) using `sysctlbyname("hw.optional.arm.FEAT_SVE2", ...)`.

## References

- [ADR-0213](0213-ssimulacra2-sve2.md) — SSIMULACRA 2 SVE2 dispatch (the path being gated)
- [core/src/meson.build:281-293](../../core/src/meson.build) — pre-fix probe site
- [core/src/arm/cpu.c:40-47](../../core/src/arm/cpu.c) — runtime `__linux__` gate this build-time fix mirrors
- Source: `req` — paraphrased: contributor reports macOS-arm build failing with "a bunch of arm sve errors"; trying on an arm GitHub runner would surface the same.
