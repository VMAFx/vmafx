# ADR-1254: Wide vector register pressure is a Win64 correctness constraint, not a performance one

- **Status**: Accepted
- **Date**: 2026-09-16
- **Deciders**: Lusoris
- **Tags**: simd, build, windows, ci

## Context

`ssim_accumulate_avx512` crashed on Windows with an access violation on a read
of address `0xFFFFFFFFFFFFFFFF`. That address is not a wild pointer: a
general-protection fault supplies no faulting linear address, so Windows fills
the field with -1. The signature identifies a misaligned aligned-access fault,
and the faulting instruction was `vmovaps %zmm28,0x1a0(%rsp)` — a
64-byte-aligned 512-bit spill addressed as a fixed offset from `%rsp`, in a
function whose prologue never realigned the frame.

The Microsoft x64 calling convention guarantees only **16-byte** stack
alignment, and its unwind contract constrains the prologue in a way that
prevents the `and $-64, %rsp` realignment gcc emits freely on SysV. gcc's
MinGW target allocates the over-aligned spill slot anyway. The result is a
function that runs correctly only when the caller happens to leave `%rsp` at
the right residue — in practice, a crash on most call paths.

Two properties make this worse than an ordinary portability bug. It is
**invisible to review**, because nothing in the C source asks for a spill;
register pressure does. And it is **invisible to CI**, because the Windows
runners do not expose AVX-512, so the Windows test leg never executes the
affected path. The defect reached a release branch and would have reached
users with AVX-512 hardware.

The behaviour is present in gcc 14.2.1 and gcc 16.2.0, and is not suppressed
by `-mstackrealign` or `-mprefer-vector-width=256`.

## Decision

We treat wide vector register pressure in x86 SIMD kernels as a **correctness**
constraint on the Windows target, not a performance tuning knob. A kernel must
not hold enough `__m512` / `__m256` values live to force the compiler into a
32- or 64-byte-aligned stack spill. Where a choice exists between hoisting
vector constants out of a loop and rebuilding them per block, we rebuild them
per block.

This is enforced mechanically rather than by review:
`scripts/ci/check-win64-stack-alignment.py` disassembles the built Windows
objects and fails the build if any function performs an aligned 256/512-bit
vector access relative to `%rsp` / `%rbp` without having realigned its frame.
It runs on the `Windows MinGW64` lane, where it inspects the exact objects that
lane produces.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rebuild vector constants per block + objdump gate (**chosen**) | Removes the spill at the source; the gate catches any recurrence in code nobody is looking at, on the exact compiler CI uses | Costs seven broadcasts per 16 pixels; the gate needs a disassembler on the Windows lane | — |
| Fix nothing, document the hazard | Zero code change | Ships a crash to every Windows user with AVX-512 | A known crash in shipped code is not a documentation problem |
| `-mprefer-vector-width=256` on the Windows lane | One build flag | Measured: does not help. The kernels use explicit `__m512` intrinsics, which the flag does not override; the spill remains | Falsified by measurement |
| `-mstackrealign` | Documented as forcing realignment | Measured: no effect on this function. The attribute realigns to 16 bytes, which is already the case | Falsified by measurement |
| Drop AVX-512 kernels on Windows | Certainly correct | Surrenders the fork's headline SIMD path on a supported platform for a compiler defect | Disproportionate |
| Keep the hoist, add `_Alignas`/`storeu` changes to the arrays | Minimal diff | Measured: the `_Alignas(64)` arrays are scalarised away entirely and are not the cause; the spill is unchanged | Falsified by measurement |
| Rely on review to keep pressure low | No tooling | The failure mode is a register-allocator decision invisible in the source, on a path CI cannot execute | Exactly the case where review does not work |

## Consequences

- **Positive**: the crash is gone, and the class of defect cannot return
  silently — a regression fails the Windows build rather than faulting on a
  user's machine. The gate covers every current and future x86 SIMD
  translation unit, not just the one that broke.
- **Negative**: a real constraint now sits on how these kernels may be written,
  and it is not expressible in the C source. The inline comment in
  `ssim_avx512.c` and this ADR are the only places it is written down, which is
  why the gate exists to enforce it mechanically.
- **Neutral / follow-ups**: the gate needs `objdump` on the MinGW lane (already
  present in the `binutils` the msys2 toolchain installs). An audit of all 30
  x86 SIMD translation units found `ssim_accumulate_avx512` to be the only
  affected function at the time of writing, so the gate starts from a clean
  tree with no baseline or suppression list.

## References

- `docs/research/2061-win64-cannot-realign-the-stack.md` — the reproduction,
  the null results, and the measurements behind each rejected alternative.
- [ADR-1253](1253-scalar-fma-not-fused-on-msvcrt.md) — the other Windows
  numerical defect found on the same lane, and the same lesson: the Windows C
  runtime and toolchain differ from the Linux one in ways that only measurement
  reveals.
- [ADR-1207](1207-feature-isa-invariance-gate.md) — the ISA-invariance gate, which
  covers score divergence between ISAs but cannot cover a fault on a path the
  runner's CPU will not execute.
- Microsoft, *x64 calling convention* — stack allocation and alignment.
- Related: `#1425`.
- Source: `req` (paraphrased: the user directed that bugs be found and fixed
  rather than left open).
