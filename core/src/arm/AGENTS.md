# AGENTS.md — core/src/arm

Orientation for agents working on aarch64 CPU-feature detection.
Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

```text
arm/
  cpu.c    # vmaf_get_cpu_flags_arm() — runtime NEON / SVE2 detection
  cpu.h    # VMAF_ARM_CPU_FLAG_* enum
```

Sister directory: [`../x86/`](../x86/) holds the equivalent x86
CPUID + XGETBV detection (verbatim from dav1d, no fork-local
modifications and so no AGENTS.md).

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md)).
- **Header is upstream-mirror at the structural level** (Netflix
  copyright on `cpu.c`); the SVE2 detection is fork-local on top.

## Rebase-sensitive invariants

- **`HWCAP2_SVE2` fork-local fallback** (T7-38). `cpu.c` defines
  `HWCAP2_SVE2 = (1UL << 1)` locally when the system header does
  not provide it. The Linux ABI value is bit 1 in `AT_HWCAP2` on
  aarch64 (per `linux/arch/arm64/include/uapi/asm/hwcap.h`); it is
  stable across kernel versions but only added to glibc in 2.33.
  The fork ships the local fallback so the build does not depend
  on a recent glibc header. **On rebase**: do not drop the
  `#ifndef HWCAP2_SVE2` guard. If glibc 2.33 becomes the project
  baseline, the fallback can be removed in a follow-up PR with
  an ADR.

- **`vmaf_get_cpu_flags_arm()` runtime SVE2 probe is gated to
  Linux on aarch64**. The `getauxval(AT_HWCAP2)` path is wrapped in
  `#if defined(__linux__) && defined(ARCH_AARCH64)`. Other OSes
  (macOS, BSDs, Windows ARM) return only the
  `VMAF_ARM_CPU_FLAG_NEON` baseline. **On rebase**: when adding a
  new ARM CPU-feature bit, mirror the gating shape — never call
  `getauxval` outside the Linux + aarch64 block.

- **`VMAF_ARM_CPU_FLAG_NEON` is unconditional on aarch64**. The
  baseline always sets the bit because NEON is mandatory on
  aarch64. Do not introduce a runtime probe — it would be a
  regression vs upstream.

## Why this matters on rebase

The SVE2 dispatch in [`../feature/arm64/`](../feature/arm64/) (currently
just `ssimulacra2_sve2.c` per ADR-0213) reads `cpu_flags &
VMAF_ARM_CPU_FLAG_SVE2` to decide whether to call into the SVE2
kernel. If the runtime probe regresses (e.g. accidentally returns
`VMAF_ARM_CPU_FLAG_NEON` only), every SVE2 kernel falls back to
NEON silently. The fork-local fallback for `HWCAP2_SVE2` exists
specifically to keep this probe working on stock Ubuntu / Debian
hosts that ship older glibc.

## Governing ADRs

- [ADR-0213](../../../docs/adr/0213-ssimulacra2-sve2.md) — SVE2
  port of SSIMULACRA 2 SIMD; consumes
  `VMAF_ARM_CPU_FLAG_SVE2`.
