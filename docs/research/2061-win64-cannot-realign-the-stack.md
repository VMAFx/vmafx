<!-- markdownlint-disable MD013 -->

# 2061 — The address that was not an address

**Date**: 2026-09-16
**Scope**: `ssim_accumulate_avx512` faulting under the Windows MinGW64 build
(ledger L-23).
**Outcome**: root cause found in the interaction between the MS x64 unwind
contract and gcc's spill-slot allocation; fixed in
`core/src/feature/x86/ssim_avx512.c`; a whole-tree audit and a CI gate
(ADR-1254).

## The reported symptom

```text
wine: Unhandled page fault on read access to FFFFFFFFFFFFFFFF
      at address 00000001400BD6E4 (thread 0138)
```

The ledger recorded this as a "read of `0xFFFFFFFFFFFFFFFF`" and filed it as a
probable out-of-bounds read in SIMD code. It is neither a read nor an address.

## What the -1 actually means

Windows reports an access violation through
`EXCEPTION_RECORD.ExceptionInformation`, where `[0]` encodes the access type
and `[1]` the faulting address. Both fields are derived from the CPU fault. A
**page fault** (#PF) supplies the faulting linear address in `CR2`; a
**general-protection fault** (#GP) does not supply one at all. For a #GP,
`[1]` is filled with `-1` and `[0]` with `0`, so every #GP surfaces as a *read*
of `0xFFFFFFFFFFFFFFFF` regardless of what the instruction was actually doing.

So the signature does not describe a bad pointer. It describes a #GP, and in
SIMD code the overwhelmingly likely #GP is a misaligned *aligned* vector
access. That reframing is the whole investigation: the search moved from "which
pointer is wrong" to "which access assumes alignment it does not have".

## The first hypothesis was wrong, and cheap to falsify

The loop bound is `for (; i + 16 <= n; i += 16)` with a scalar tail, so the
16-wide body cannot run off the end. The workspace is `malloc`-ed, which gives
16-byte alignment, and every load of it uses the `loadu` form. Reading the
source found nothing.

Cross-compiling it did. The pre-fix function at `-O2`:

```asm
0000000000000520 <ssim_accumulate_avx512>:
 520:   push   %r15 ... push %rbx          (8 pushes)
 52c:   sub    $0x2a8,%rsp
 533:   vmovaps %xmm6,0x200(%rsp)          (ABI save of a non-volatile xmm — fine)
 ...
 721:   vmovaps %zmm28,0x1a0(%rsp)         <-- faults
 72c:   vmovaps %zmm29,0x160(%rsp)
 737:   vmovaps %zmm30,0x120(%rsp)
 742:   vmovapd %zmm31,0xe0(%rsp)
 74d:   vmovapd %zmm26,0xa0(%rsp)
```

Five 64-byte-aligned 512-bit stores at fixed offsets from `%rsp`, in a prologue
containing **no** stack realignment. The same source compiled for Linux opens
with:

```asm
    4414:   lea    0x8(%rsp),%r10
    4419:   and    $0xffffffffffffffc0,%rsp     <-- realign to 64
    441d:   push   -0x8(%r10)
```

That is the difference. On SysV gcc realigns `%rsp` and the aligned spills are
safe. On Win64 it does not, because the unwind contract constrains what a
prologue may do to `%rsp`, and gcc's MinGW target has no way to express a
realigned frame. It allocates the over-aligned slot anyway. The MS x64 calling
convention guarantees 16-byte alignment, so `0x1a0(%rsp)` is 64-byte aligned
only when the caller happens to leave `%rsp` at one residue in four.

## Confirming it, rather than believing it

A 40-line probe linked directly against the cross-compiled object:

```text
$ wine probe.exe                       # pre-fix
  pad=  0  &local % 64 = 56
wine: Unhandled page fault on read access to FFFFFFFFFFFFFFFF
      at address 0000000140001D61 (thread 0024), starting debugger...
```

`0x140001D61` resolves, in that binary's own disassembly, to
`vmovaps %zmm28,0x1a0(%rsp)` inside `ssim_accumulate_avx512` — the instruction
predicted above, not merely a function name and an offset.

A caveat worth recording: the probe varies an `alloca` pad intending to sweep
all four residues of `%rsp mod 64`, and it does not succeed — the reported
`&local % 64` stays at 56, because gcc is free to re-align the local
independently of the pad. The probe therefore demonstrates crash versus
no-crash, which is what the conclusion rests on. It does not demonstrate a
per-residue sweep, and should not be read as doing so.

## Three null results

Each of these looked plausible and each is false. They are recorded because
the next person will think of them too.

| Hypothesis | Measurement | Result |
| --- | --- | --- |
| The `_Alignas(64) double t_lv[16]` arrays are the over-aligned objects | Dropped `_Alignas`, switched the four stores to `_mm512_storeu_pd`, recompiled | **No change.** Still 5 aligned spills. The arrays are scalarised away entirely and never reach memory; they are not the cause |
| gcc simply cannot handle over-aligned locals on MinGW | Compiled a minimal function with an `_Alignas(64)` array in isolation | **Falsified.** gcc emits `lea 0x3f(%rsp),%rax; and $-64,%rax` and rounds a scratch pointer — correct. It handles *locals* fine. Only *spill slots* are broken |
| A newer gcc has fixed it | Built the same TU with Fedora's MinGW gcc 14.2.1 and Arch's 16.2.0 | **Unfixed in both.** 5 aligned spills, 0 realignments, identical |

And two flags that sound like the answer:

| Flag | Result |
| --- | --- |
| `-mstackrealign` | No effect. It realigns to 16 bytes, which already holds |
| `-mprefer-vector-width=256` | No effect. The kernel uses explicit `__m512` intrinsics, which the flag does not override |

## The fix, and why it is this one

The spill exists because the function holds seven `__m512`/`__m512d` broadcast
constants live across the whole loop, on top of the block body's own working
set — past 32 live vector values, so five get spilled. Rebuilding those
constants inside the block instead of hoisting them removes the pressure:

```text
pre-fix : 249 instructions, 5 aligned zmm stack accesses, 0 realignments
post-fix: 261 instructions, 0 aligned zmm stack accesses, 0 spills at all
```

Twelve extra instructions per 16 pixels — seven broadcasts that now stay in
registers. No arithmetic changes, so bit-exactness against the scalar reference
is preserved by construction rather than by assertion.

Verification, in the order it was run:

1. `wine probe.exe` on the fixed object completes instead of faulting.
2. Scores over the 48-frame Netflix fixture at `--precision=max` are
   **bit-identical** to the pre-fix build for `float_ssim` and `float_ms_ssim`.
3. A deliberate perturbation (`2.0` → `2.0000001` in the rebuilt constants)
   **moves** those scores — so the edited block is genuinely live in the binary
   the comparison measured, and step 2 is not a vacuous pass.
4. 150/150 unit tests under ASan + UBSan, including the ADR-1207
   ISA-invariance gate.
5. The Netflix golden-data gate.

## Why this needed a gate and not just a fix

The two properties that let this reach a release branch are both structural:

- **Nothing in the C source asks for a spill.** It is a register-allocator
  decision. No reviewer reading `ssim_avx512.c` could have seen it, and no
  reviewer reading the fixed version can see that adding one more live vector
  constant would bring it back.
- **CI cannot execute the path.** GitHub's Windows runners do not expose
  AVX-512, so the Windows test leg runs the AVX2 or scalar path and passes
  while the AVX-512 path is a crash. The ADR-1207 ISA-invariance gate has the
  same blind spot for the same reason: it compares scores, and a path that
  never runs produces no score to compare.

`scripts/ci/check-win64-stack-alignment.py` closes both. It disassembles the
built Windows objects and fails on any aligned 256/512-bit vector access taken
relative to `%rsp`/`%rbp` in a frame that was never realigned — reading what
the compiler emitted rather than what the source says, on the exact objects the
MinGW lane produces. It was validated against the pre-fix object (5 findings,
exit 1) and the fixed one (clean, exit 0).

## Whole-tree audit

Every x86 SIMD translation unit was cross-compiled for Windows and scanned.
**`ssim_accumulate_avx512` was the only affected function** — the other
kernels either stay under the pressure threshold or never use aligned
frame-relative vector accesses. The gate therefore starts from a clean tree
with no baseline file and no suppression list, which is the state a ratchet
should start in.

Worth noting that the 32-byte case is equally exposed in principle: an AVX2
kernel that spilled a `ymm` register would hit the same fault, since 16-byte
alignment is all the ABI gives. None currently does, and the gate covers
`ymm` as well as `zmm` so that remains true.

## Two transferable lessons

**A fault address is a report from a mechanism, not a fact about your pointer.**
`0xFFFFFFFFFFFFFFFF` was read here for a full session as "something dereferenced
-1". It meant "the CPU did not supply an address", which is a different
question with a much smaller answer set.

**Platform ABIs differ in what they permit the compiler to do, not only in
where arguments go.** The Win64 unwind contract is why gcc cannot realign, and
that is invisible from the C source, invisible from the calling convention as
usually summarised, and only appears in the emitted prologue. ADR-1253 found
the same shape one layer up — `fmaf()` fused on one C runtime and not another —
on this same CI lane, within the same week.
