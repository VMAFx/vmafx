# AGENTS.md — core/src/arm

Orientation: agents working on aarch64 CPU-feature detection.
Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

```text
arm/
  cpu.c    # vmaf_get_cpu_flags_arm() — runtime NEON / SVE2 detection
  cpu.h    # VMAF_ARM_CPU_FLAG_* enum
```

Sister directory: [`../x86/`](../x86/) holds equivalent x86 CPUID + XGETBV
detection (verbatim from dav1d, no fork-local modifications -> no AGENTS.md).

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md)).
- **Header = upstream-mirror at structural level** (Netflix copyright on
  `cpu.c`); SVE2 detection = fork-local on top.

## Rebase-sensitive invariants

- **`HWCAP2_SVE2` fork-local fallback** (T7-38). `cpu.c` defines
  `HWCAP2_SVE2 = (1UL << 1)` locally when system header lacks it. Linux ABI
  value = bit 1 in `AT_HWCAP2` on aarch64 (per
  `linux/arch/arm64/include/uapi/asm/hwcap.h`); stable across kernel versions,
  added to glibc in 2.33. Fork ships local fallback so build avoids dependency
  on recent glibc header. **On rebase**: do not drop `#ifndef HWCAP2_SVE2`
  guard. If glibc 2.33 becomes project baseline, remove fallback in follow-up
  PR with ADR.
- **`vmaf_get_cpu_flags_arm()` runtime SVE2 probe gated to Linux on aarch64**.
  `getauxval(AT_HWCAP2)` path wrapped in
  `#if defined(__linux__) && defined(ARCH_AARCH64)`. Other OSes (macOS, BSDs,
  Windows ARM) return only `VMAF_ARM_CPU_FLAG_NEON` baseline. **On rebase**:
  when adding new ARM CPU-feature bit, mirror gating shape — never call
  `getauxval` outside Linux + aarch64 block.
- **`VMAF_ARM_CPU_FLAG_NEON` unconditional on aarch64**. Baseline always sets
  bit; NEON mandatory on aarch64. Do not introduce runtime probe — regression
  vs upstream.

## Why this matters on rebase

SVE2 dispatch in [`../feature/arm64/`](../feature/arm64/) (currently just
`ssimulacra2_sve2.c` per ADR-0213) reads `cpu_flags & VMAF_ARM_CPU_FLAG_SVE2`
to decide call to SVE2 kernel. If runtime probe regresses (e.g. returns
only `VMAF_ARM_CPU_FLAG_NEON`), every SVE2 kernel falls back to NEON silently.
Fork-local fallback for `HWCAP2_SVE2` exists specifically to keep probe
working on stock Ubuntu / Debian hosts with older glibc.

## Governing ADRs

- [ADR-0213](../../../docs/adr/0213-ssimulacra2-sve2.md) — SVE2 port of
  SSIMULACRA 2 SIMD; consumes `VMAF_ARM_CPU_FLAG_SVE2`.
