<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1467: `ciede.c` is built so that its `powf()` calls reach the C library; a clang build returns the GCC build's `ciede2000`

- **Status**: Accepted
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `build`, `numerics`, `ciede`, `clang`, `testing`, `rc3`, `fork-local`

## Context

A clang build and a GCC build of the CPU `ciede` extractor returned different
`ciede2000` scores on the same machine with the same C library, on x86-64 and
on aarch64 (`T-CIEDE-CLANG-POWF-BUILTIN-2026-10-02`). On ten fixtures (180
frames: the Netflix 576x324 pair at 8, 10, 12 and 16 bits and as 10-bit 4:2:2,
Sparks, both 1920x1080 checkerboard pairs, Big Buck Bunny at 1920x1080 and
3840x2160) 65 frames differed, by at most 1.96e-11.

The cause is one call. `get_r_sub_t()` in `core/src/feature/ciede.c` writes
`powf(degrees, 2)`. Compiled one form per function with the flags of the
build (`-O3 -std=c23 -ffp-contract=off`), GCC 16 and clang 22 on both
architectures and icx 2026.0 (`-fp-model=precise`) do this to the power forms
the file uses:

| Form in `ciede.c` | GCC | clang | icx |
|---|---|---|---|
| `powf(x, 2)` | calls `powf` | multiplies | multiplies |
| `powf(x, 7)` | calls `powf` | calls `powf` | calls `powf` |
| `powf(25., 7)` | constant | constant | constant |
| `pow(x, 2)` (13 uses) | calls `pow` | multiplies | multiplies |
| `pow(x, 7)`, `pow(x, 2.4)`, `pow(x, 1.0 / 3.0)` | calls `pow` | calls `pow` | calls `pow` |
| `pow(25, 7)` | constant | constant | constant |

So clang replaces a power of two by a product, in `float` and in `double`.
Whether that changes a value depends on the C library:

- The product `x * x` is the correctly rounded square. glibc 2.44's
  `powf(x, 2)` is not always: it returns the other neighbouring `float` on
  4966 of 4 000 000 sampled arguments of `get_r_sub_t()` (0.12 %), every one
  an exact tie, and on 1597 of the 1 048 576 hues `test_ciede_powf_call`
  sweeps.
- glibc's `pow(x, 2)` equals the product on 100 million sampled arguments
  (float-valued `x`, and `l_bar - 50` for `l_bar` across [0, 100] and across
  [0, 8)). The `double` squares of the file are exact or far from a tie, so
  that replacement changes nothing.

A GCC build whose source reads `degrees * degrees` returns the clang build's
values on 180 of 180 frames. The `float` square in `get_r_sub_t()` is therefore
the whole difference between the two compilers.

The GCC build is the reference: `make test-netflix-golden` builds with GCC
where it is installed (`scripts/ci/setup-golden-build.sh`), and
[ADR-0024](0024-netflix-golden-preserved.md) keeps the values that gate checks.
The three GPU twins that run the CPU's arithmetic compute this term
as the correctly rounded square, each documented as differing from the CPU
"where the host's `powf` rounds the other way": `ciede_cuda` through
`CIEDE_POWF` ([ADR-1426](1426-cuda-ciede-cpu-arithmetic.md)), `ciede_sycl` and
`ciede_hip` as `degrees * degrees` in `feature/ciede_ff_math.h`
([ADR-1436](1436-sycl-ciede-cpu-arithmetic.md),
[ADR-1448](1448-hip-ciede-cpu-arithmetic.md)). Their bound in `LIBM_TWINS`
(`1e-9`) was measured against the GCC build.

## Decision

`ciede.c` is built in a library of its own, `libvmaf_ciede_static_lib`, with
`vmaf_strict_fp_args` and a new list `vmaf_libm_call_args`. The list is
`-fno-builtin-powf` for clang and Apple's clang and empty for every other
compiler, so the `powf()` calls of the file are calls of the C library under
the compilers that share that library with a GCC build. The source is not
changed, and neither is what a GCC build computes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Write `degrees * degrees` in `ciede.c` | The source states the operation; no flag; every compiler and every C library agree on this term; it is the form of the three GPU twins | Measured: moves the GCC build on 65 of 180 frames, by up to 1.96e-11. The reference build changes, in a file of Netflix's | The GCC build's scores must not move by one bit. This is the alternative to revisit if the reference is ever redefined |
| The flag for every compiler that accepts it, GCC included | One rule for all | GCC emits the call already; with the flag it also evaluates `powf(25., 7)` at run time, so its object changes and gains a call per pixel | GCC's object stays byte-identical instead (measured) |
| The flag for icx as well | "Every compiler that folds" is a simpler rule | icx links Intel's math library, whose `powf(x, 2)` differs from the product on 3041 of 4 000 000 arguments and from glibc's: an icx build differs from a GCC build in `ciede` whatever this call does (5.7e-12, [ADR-1415](1415-x86-simd-libraries-strict-fp.md)). The flag would only move its CPU away from the value the SYCL twin computes | No build for icx to agree with |
| `-fno-builtin-pow` as well | Covers the 13 `double` squares | Measured to change nothing with glibc 2.44; it would turn `pow(25, 7)` into two more calls per pixel | No measured effect to remove |
| `-fno-builtin` for the file | Covers any future replacement | Turns `sqrt`, `fabs` and `memcpy` into calls; unmeasured | Wider than the cause |
| A `no_builtin("powf")` attribute or a volatile function pointer in `get_r_sub_t()` | Scoped to one function | Compiler-specific text, or an indirect call, in a file of Netflix's; a source diff on every upstream sync | The build already has a place for per-file FP policy |
| Leave it and document the difference | No change | A clang build and a GCC build of the same source on the same machine disagree | It is the defect |

## Consequences

- **Positive**: a clang build returns the GCC build's `ciede2000` on 180 of
  180 measured frames on x86-64 and on aarch64 (115 of 180 before), and the
  17 CPU extractors and `vmaf_v0.6.1` of an x86-64 clang build match the GCC
  build on 3355 of 3355 values (3350 before).
- **Positive**: a GCC build is unchanged. The disassembly and relocations of
  `ciede.c` are identical with and without the library's arguments on x86-64
  and aarch64, and its reports are byte-identical on both.
- **Positive**: `ciede.c` is built without contraction on every architecture,
  so `test_ciede_device_math` (the host replay of the CUDA kernel's
  arithmetic) now runs on aarch64 too.
- **Negative**: a clang build calls `powf` four times per pixel pair where it
  called it once and multiplied; `powf(25., 7)` is evaluated at run time. The
  measured cost is in `docs/metrics/features.md` (CIEDE2000).
- **Negative**: the CPU extractor of a clang build now differs from the GPU
  twins on the pixels where glibc's `powf(x, 2)` rounds the other way, as the
  GCC build's always did. That is the figure the `LIBM_TWINS` bound was
  measured with (1.4e-11 at 3840x2160 against `1e-9`); the bound is unchanged.
- **Neutral / follow-ups**:
  - `core/test/test_ciede_libm_call_args.py` executes the policy for every
    compiler id, checks that one library holds `ciede.c` with the list and
    that the two tests below are built with it, and reads this build's compile
    command.
  - `core/test/test_ciede_powf_call.c` compares `get_r_sub_t()` with the same
    expression called through a volatile function pointer over 5.2 million
    values; with the list emptied a clang build fails it on 4827 of them.
  - `test_ciede_device_math` is built with the list too. With the list taken
    from the library only, it fails on its 640x360 16-bit case (3.1e-12), so it
    guards the library's object.
  - An icx build is unchanged in value: `ciede.c` gains `-ffp-contract=off`
    there, which reorders instructions on a baseline without a fused
    multiply-add, and its `ciede2000` is identical on 180 of 180 frames before
    and after. It differs from a GCC build on 68 of them, by at most 1.4e-11,
    before and after.
  - Not measured: Apple's clang against Apple's math library. The policy is
    stated for it because a GCC build on macOS calls the same library; the
    macOS CI lanes run both tests.
  - A Windows build calls the UCRT's `powf()` under every compiler; the policy
    names no flag for `cl.exe`, clang-cl or icx-cl.

## References

- [ADR-0024](0024-netflix-golden-preserved.md) — the Netflix golden values.
- [ADR-1426](1426-cuda-ciede-cpu-arithmetic.md),
  [ADR-1436](1436-sycl-ciede-cpu-arithmetic.md),
  [ADR-1448](1448-hip-ciede-cpu-arithmetic.md) — the GPU twins and their
  `LIBM_TWINS` bound.
- [ADR-1415](1415-x86-simd-libraries-strict-fp.md) — an icx build and
  Intel's math library.
- `docs/state.md`: `T-CIEDE-CLANG-POWF-BUILTIN-2026-10-02`.
- Source: `req` (coordinator, 2026-10-02): "Isolate which `powf` / `pow` call clang folds (exponent constants such as 2, 0.5, 7: clang rewrites `pow(x, 2.0)` to `x * x` and `powf(x, 0.5f)` to `sqrtf`, GCC does or does not: measure, do not assume) and make the source say what is meant so both compilers compute the same thing: prefer writing the operation explicitly (`x * x`, `sqrt`) over a per-file flag, provided the GCC build's scores do not move by one bit (GCC is the golden reference: 271 / 12 must hold and the x86 GCC reports stay byte-identical); if the explicit form would move GCC, use the flag on clang for that TU through the existing strict-FP mechanism and say why. The three ciede GPU twins carry a `LIBM_TWINS` bound derived against the glibc CPU: check they are unaffected (the CUDA / HIP / SYCL device code has its own copies of the expressions: list whether each mirrors the folded or the unfolded form)."
