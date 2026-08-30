<!-- markdownlint-disable MD060 -->
# ADR-0213: SSIMULACRA 2 SVE2 SIMD parity

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: lusoris, Claude
- **Tags**: simd, arm64, sve2, ssimulacra2, qemu, ci

## Context

Research-0016 (IIR blur SIMD) and Research-0017 (`picture_to_linear_rgb`
SIMD) both closed with the footnote *"SVE2 port — deferred pending CI
hardware"*. The fork has shipped AVX2 / AVX-512 / NEON ports and a CUDA

- SYCL twin (ADR-0161, ADR-0162, ADR-0163, ADR-0206) for SSIMULACRA 2;
SVE2 was the last gap on the SIMD matrix.

Per user direction (T7-38, 2026-04-28) the deferral is overruled:
develop the SVE2 port locally against `qemu-aarch64-static -cpu max` so
the dispatch table covers SVE2-capable Cortex-A720 / Neoverse class
silicon as soon as it shows up. CI hardware is not on the critical
path — qemu validates correctness today and a native-aarch64 runner
remains a follow-up perf concern.

The bit-exactness invariants from ADR-0138 / ADR-0139 / ADR-0140 are
non-negotiable: every SIMD path must produce byte-identical output to
the scalar reference under `memcmp` on the SSIMULACRA 2 test fixtures.

## Decision

We will ship an aarch64 SVE2 sister TU
(`core/src/feature/arm64/ssimulacra2_sve2.c`) that mirrors the NEON
port lane-for-lane, with every kernel locked to a fixed 4-lane
predicate (`svwhilelt_b32(0, 4)`). The wider lanes that SVE2 hardware
may expose stay false, so the arithmetic order and per-lane reductions
are byte-identical to the NEON sibling regardless of the runtime
vector length. NEON remains the fallback; SVE2 is purely additive
behind a `getauxval(AT_HWCAP2) & HWCAP2_SVE2` runtime probe.

The dispatch table in `ssimulacra2.c::init_simd_dispatch` overrides
NEON with SVE2 when the bit is set. Test
`core/test/test_ssimulacra2_simd.c` picks SVE2 over NEON in its
`pick_*` helpers and prints the selected path to stderr so qemu vs
native runs are distinguishable in CI logs. Build-time gating uses
a `cc.compiles()` probe in `core/src/meson.build` that exercises
`-march=armv9-a+sve2` against `<arm_sve.h>` — failure leaves
`HAVE_SVE2` unset and the binary falls back to NEON.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Fixed 4-lane SVE2 (chosen)** | Byte-exact with NEON irrespective of VL; trivially lifts the NEON audit trail; no new bit-exactness ADR needed | Wastes lanes >4 on 256-bit / 512-bit hardware (perf, not correctness) | Best correctness/effort ratio; perf gain from wider lanes is a follow-up |
| **Variable-length SVE2 (`svcntw()`-driven loops)** | Maximises throughput on wide SVE2 hardware (Neoverse V2 = 256-bit, A64FX = 512-bit) | Different reduction order per VL — fails ADR-0138 byte-exact contract; would need a new tolerance ADR + snapshot regen | Breaks the existing audit trail; perf upside not justified now |
| **SVE1 fallback alongside SVE2** | Covers older Neoverse N1 / Cortex-A510 | SVE1 lacks several SVE2 ops; would mean two sister TUs for marginal coverage; the install base of SVE-only-no-SVE2 silicon is small | Out of scope for T7-38; revisit if a target platform demands it |
| **SME (Scalable Matrix Extension) port** | Future-proof; tile registers ideal for matmul-style XYB conversion | Available on a tiny set of M4 / future Neoverse parts; toolchain support nascent (GCC 14+ only) | Defer — re-evaluate when SME hardware is mainstream |
| **Stay with "deferred pending CI hardware"** | Zero work | Backlog drift; user explicitly overruled the deferral | Per user direction 2026-04-28 |

## Consequences

- **Positive**:
  - SSIMULACRA 2 covers every SIMD ISA the fork supports — no remaining
    gaps in the metric backend matrix's ARM column.
  - Closes Research-0016 / Research-0017 SVE2 follow-up tickets.
  - Forward-compatible with future native aarch64 CI runners; no code
    change needed when we add one.
- **Negative**:
  - Build matrix grows by one cross-file
    (`build-aux/aarch64-linux-gnu-sve2.ini`) and one optional static
    library (`arm64_ssimulacra2_sve2`).
  - `core/src/arm/cpu.c` now depends on `<sys/auxv.h>` on Linux —
    portable enough but introduces a Linux-only fast path.
- **Neutral / follow-ups**:
  - Native aarch64 perf runner (T7-?? backlog) — replace qemu timings
    with real numbers once silicon is routinely available.
  - Variable-length SVE2 perf optimisation — gated on a separate ADR
    plus snapshot regen if it introduces VL-dependent drift.
  - SME port — track separately when the hardware shows up.

## References

- Research-0016 (IIR blur SIMD), Research-0017 (`picture_to_linear_rgb`
  SIMD) — both close their "SVE2 deferred" follow-ups.
- ADR-0138 (SIMD bit-exactness via per-lane scalar reduction),
  ADR-0139 (SSIM SIMD bit-exact double accumulator), ADR-0140
  (SSIMULACRA 2 SIMD bit-exact contract) — the invariants this port
  preserves.
- ADR-0161 (SSIMULACRA 2 SIMD phase 1), ADR-0162 (IIR blur),
  ADR-0163 (`picture_to_linear_rgb`) — the NEON sibling this port
  mirrors.
- ADR-0141 (touched-file lint cleanup) — function-size NOLINT
  citations inline in the new TU follow the same pattern as the
  NEON sibling.
- Source: `req` — user direction 2026-04-28: *"do not defer, develop
  locally against `qemu-aarch64-static`. Mirror existing NEON ports
  with predicated lanes; correctness via `qemu-aarch64-static
  -cpu max,sve=on,sve2=on ./build-arm64/tools/vmaf` against Netflix
  golden pairs."*
