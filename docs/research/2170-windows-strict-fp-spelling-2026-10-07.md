# Research-2170: Windows icx-cl and icpx strict FP spelling — 2026-10-07

## Finding

On the gate commit of the MSVC warnings-as-errors work (job 112842753545 of run
37635551653), `Windows MSVC+SYCL` printed 2,268 of its 4,361 warnings as
`overriding '-ffp-model=precise' option with '-ffp-contract=off'
[-Woverriding-option]`, from two places:

- every C and C++ compile by icx-cl (1,955), from the project-wide
  `vmaf_strict_fp_args` of `intel-llvm-cl`, `/fp:precise /Qfma-`: icx-cl maps
  `/fp:precise` to `-ffp-model=precise` and `/Qfma-` to `-ffp-contract=off`;
- every SYCL compile and device link by icpx (the MSVC build compiles its SYCL
  translation units and their explicit device link, ADR-1364, with the GNU-syntax
  icpx), from `sycl_strict_fp_args`, which kept the two-flag spelling on that
  build.

ADR-2170 removed the same override for the Unix icx and icpx by spelling
`-fno-fast-math -fcomplex-arithmetic=full` between the model and the
contraction flag. The question here is whether the same line, through `/clang:`
for icx-cl and verbatim for the Windows icpx, sets the same floating-point
state on the Windows target.

## Method

oneAPI 2026.0.0 (`icx-cl` and `icpx` of the Linux installation, which compile
for `--target=x86_64-pc-windows-msvc` without the Windows SDK as long as the
translation unit includes no header). The Windows leg pins 2025.3.0.372; the
CI run of the pull request is the check on that version.

- icx-cl, old `/fp:precise /Qfma-` against new `/fp:precise
  /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full
  /clang:-ffp-contract=off`: the `-cc1` line of `-###`, the predefined macros
  (`/E /clang:-dM`), and the objects of a corpus of `a * b + c` in double and
  float, `a / b + a * b`, a dot-product loop, a float/double mix and complex
  multiply and divide (double and float), at `/O2`, `/O3`, `/O3 /arch:AVX2`
  and `/O3 /arch:CORE-AVX512`.
- icpx with `-fsycl -fsycl-targets=spir64_gen,spir64`, old `-fp-model=precise
  -ffp-contract=off` against new `-fp-model=precise -fno-fast-math
  -fcomplex-arithmetic=full -ffp-contract=off`, both followed by
  `-foffload-fp32-prec-div -foffload-fp32-prec-sqrt`: the `-cc1` lines of the
  host and both device passes, the `-fsycl-link` expansion, the predefined
  macros, and the `-fsycl-device-only` bitcode of the same corpus.

## Results

| Check | icx-cl | icpx (SYCL) |
| --- | --- | --- |
| Override warning | 1 per compile old, 0 new | 3 per compile and 1 per device link old, 0 new |
| `-cc1` lines | equal apart from `-fcomplex-arithmetic=full` | equal apart from `-fcomplex-arithmetic=full` (host, spir64_gen, spir64, device link) |
| Predefined macros | 402, equal | 970, equal |
| Output | objects byte-identical at all four settings | device bitcode byte-identical |

The new line is the same arithmetic. `-fcomplex-arithmetic=full` restates the
complex-arithmetic mode `-fp-model=precise` sets and `-fno-fast-math` would
otherwise reset, as on Linux.

## Consequence

`core/src/meson.build` spells the icx-cl strict line `/fp:precise
/clang:-fno-fast-math /clang:-fcomplex-arithmetic=full /clang:-ffp-contract=off`
and gives the MSVC build's icpx the same reset as Linux.
`core/test/test_strict_fp_compiler_args.py` executes both and requires every
icx-cl compile command to end its FP flags on `/clang:-ffp-contract=off`.
