<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1495: icx and icpx builds link glibc's libm, not Intel's libimf

- **Status**: Accepted
- **Date**: 2026-10-03
- **Deciders**: lusoris
- **Tags**: `build`, `numerics`, `icx`, `sycl`, `testing`, `rc3`, `fork-local`

## Context

The CPU extractors of a build made with Intel's compilers (`CC=icx
CXX=icpx`: every SYCL build, the `vmaf` of the dev image, the `Linux Intel
LLVM` CI job) returned other scores than a GCC build in the last digits
(`T-ICX-LIBIMF-HOST-MATH-2026-10-01`). After ADR-1415 and ADR-1461 removed
the contraction differences, what was left was the math library. Measured on
a reference workstation with icx 2026.0 and GCC 16.2.1, glibc 2.44, `--backend cpu
--precision max --threads 4`, on the Netflix 576x324 pair (48 frames), both
1080p checkerboard pairs, BBB 1280x720 (48 frames; the clip
`testdata/generate.sh` makes, whose scores equal `testdata/scores_cpu_720.json`
on 720 of 720 values) and BBB 3840x2160 (200 frames): 268 of 13288 values
differed. `integer_adm_scale1` by 7.9e-8 on BBB 1280x720 frame 26,
`float_adm`'s `adm_scale3` with `adm_f1s3=2.25:adm_f2s0=0.3` by 7.5e-8 on
Netflix frame 27, `ciede2000` on 191 of 200 BBB 3840x2160 frames (6.3e-12),
`psnr`, `psnr_hvs` and their chroma outputs by up to 1.4e-14, and the default
model's `vmaf` by up to 1.3e-7.

How the Intel library got in, read off the driver (`icx -###`, the same in
2026.0 and 2026.1.1) and the linked binaries:

- The driver appends `-lsvml -lirng -limf -lm` to every link it runs, and
  rewrites a `-lm` it is given into `-limf -lm`. Meson's `math_lib` is such a
  `-lm`, so `libvmaf.so` needed `libimf.so`, did not need `libm.so.6`, and
  imported `log10`, `pow`, `powf`, `log2f`, `exp`, `log` and 24 other math
  functions (`atan2`, `cbrt`, `tgamma`, `sqrt`, ...) without a glibc symbol
  version. Putting `-lm` first cannot work.
- An executable takes the Intel libraries statically. The installed man page
  (`/opt/intel/oneapi/compiler/2026.0/share/man/man1/icx.1`), on
  `-static-intel`: "Default: ON. Intel(R) libraries are linked in statically
  with the following exceptions: [...] The Intel(R) libraries are linked in
  dynamically when you specify option shared." The icx-built `vmaf` therefore
  carried libimf's copies of all 30 math functions `libvmaf.so` imports and
  exported them; with `LD_DEBUG=bindings` the loader binds `libvmaf.so.3`'s
  `exp`, `log`, `pow`, ... to `tools/vmaf`. A program built with GCC binds the
  same library to glibc first: the dev image's `ffmpeg` binds the icx-built
  `libvmaf.so.3`'s six functions to `libm.so.6`. Which library an icx build's
  extractors used depended on the program that loaded them.

## Decision

Every C and C++ link whose linker is Intel LLVM (`cc.get_id()` or
`cxx.get_id()` is `intel-llvm`) gets `-no-intel-lib=libimf`, as Meson project
link arguments declared in `core/src/meson.build` between the markers `BEGIN /
END VMAF host math library link policy`, directly after the strict FP policy
and above the first target. The library, the tools and the tests then take the
math functions from glibc's `libm`, as a GCC or clang build does.

The option, as the installed version documents it. `icx --help` of 2026.0 and
of 2026.1.1: "`-no-intel-lib=<value>` Restrict linking of Intel specific
libraries. Valid arguments are libirc, libimf, libirng, libsvml". The man page
of 2026.0: "-no-intel-lib[=library] (L*X only) Disables linking to specified
Intel(R) libraries, or to all Intel(R) libraries. [...] libimf Disables
linking to the Intel(R) oneAPI DPC++/C++ Compiler Math library. [...] NOTE:
This option only applies to host compilation. When offloading is enabled, it
does not impact device-specific compilation." (The 2026.1.1 image carries no
man page; its `--help` lists the option with the same text, and its driver
drops `-limf` the same way.)

`libsvml`, `libirc` and `libintlc` stay linked. The man page, on
`-fimf-use-svml`: "Default: false. Math library functions are implemented
using the Intel(R) oneAPI DPC++/C++ Compiler Math Library, though other
compiler options may give the compiler the flexibility to implement math
library functions with either LIBM or SVML." With icx's default fast model
the vectoriser turns a loop over `powf` / `log2f` / `exp` / `log10` into
`__svml_*` calls; under `vmaf_strict_fp_args` (`-fp-model=precise
-ffp-contract=off`, ADR-1461) it emits none, at `-O3` and with `-mavx2 -mfma`
or `-mavx512f` too. Neither the library of a CPU icx build nor that of a SYCL
build imports an `__svml_` symbol, before or after this change.

`core/test/test_icx_system_libm.py` (suite `fast`) reads the build it runs in:
`libvmaf.so` needs `libm.so.6` and not `libimf.so`, imports the six functions
with glibc versions and no `__svml_` routine, `vmaf` defines none of them, and
`vmaf --version` under `LD_BIND_NOW=1 LD_DEBUG=bindings` binds every
reference to them to `libm.so.6`. On a build whose compilers are not Intel
LLVM the binary checks skip with the reason. `test_strict_fp_compiler_args`
executes the policy block for seven compiler pairs and pins the two link
argument lines.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `-lm` ahead of the driver's libraries (link order) | No Intel option | The driver rewrites a given `-lm` into `-limf -lm` (measured, 2026.0 and 2026.1.1) | Does not work |
| `-shared-intel` | Stops the static copies in the executables | `libimf.so` stays the first math library of the link: the library and the executables bind to it | Changes how libimf is linked, not whether |
| `-nodefaultlibs` or `-Wl,-lm` around the driver | Bypasses the driver | Drops libc, libgcc and the SYCL runtime libraries the driver adds, or hides the intent in a linker flag the driver does not know about | Fragile |
| `-no-intel-lib` without a value (all four libraries) | Nothing Intel-specific on the host link | Not needed for the scores; `libirc` holds `_intel_fast_memcpy`, which icx-compiled objects call | Wider than the defect |
| Also pass the option on the compile lines | The compiler would no longer be told libimf is available (`-fintel-libimf-allowed` leaves the cc1 line) | The 12 math-heavy translation units measured (`adm_tools.c`, `ciede.c`, `psnr_hvs.c`, `psnr.c`, `integer_adm.c`, `float_adm.c`, `speed.c`, `ssimulacra2.c`, `cambi.c`, `vif_tools.c`, `svm.cpp`, `predict.c`) compile to the same machine code with and without it; a call into a libimf-only entry point would fail the link (`--no-undefined`) rather than pass silently | No effect; noise on every compile line |
| Shared routines with defined rounding for the affected calls (as `ssimulacra2_math.h` does for `cbrtf`) | Independent of any C library | Every CPU extractor that calls `pow`, `log10`, `exp`, ...; changes the scores of the GCC build, which the GPU twins and the snapshots are measured against | The difference is a link property; the reference stays glibc |
| **`-no-intel-lib=libimf` on every Intel LLVM link (chosen)** | One documented driver option, host only; the library and every executable bind to glibc whatever program loads them | The host link loses Intel's math library, which was not faster here (measured below) | Chosen |

## Consequences

- **Positive**: an icx build returns a GCC build's CPU scores. Same
  measurement as above, after: 13288 of 13288 values identical (`psnr`,
  `psnr_hvs`, `ciede`, integer `adm`, `float_adm` plain and with
  `adm_f1s3=2.25:adm_f2s0=0.3`, and the default model, on the five fixtures).
  In the dev container (icx 2026.1.1 against GCC 15.2.0, glibc 2.43, BBB
  3840x2160 cut to 20 frames): 5368 of 5368. The CPU half of a SYCL build is
  the GCC build too, so a SYCL twin is now exact against the GCC build's CPU
  extractor, not only against its own build's.
- **Negative**: none measured; the icx build got faster. Time per frame on a
  48-frame 1920x1080 clip (`.corpus/bench-data/ref_1920x1080.yuv`, local
  corpus), `--threads 4` pinned to four cores, median of three interleaved
  runs, load average 11 to 13 from other work on the host: the default model
  4.35 ms before and 3.97 ms after; `psnr`, `psnr_hvs`, `ciede`, `adm` and
  `float_adm` together 105.45 ms before and 63.71 ms after (the GCC build:
  70.76 ms). All of the change is `ciede` (95.63 ms before, 54.31 ms after,
  alone): its math calls are faster in glibc 2.44 than through libimf here;
  `psnr_hvs`, `psnr`, `adm` and `float_adm` alone moved by less than 2 %.
- **Neutral / follow-ups**:
  - SYCL device code is not touched. The compile commands of an icx build are
    the same before and after (1599 of 1599 entries of
    `compile_commands.json`); only the link arguments gain the option. An
    icpx link of a separately compiled SYCL object (spir64_gen for dg2-g11 and
    spir64, the precision pair) runs 20 driver jobs, and only the host `ld`
    line differs, by `-limf`: the device link that builds the SPIR-V image is
    the same. On the Arc A380 a SYCL build with this change (icx
    2026.0, AOT for dg2-g11) passes the parity gate over all 24 features on
    the Netflix pair, both checkerboards, BBB 1280x720 and 20 frames of BBB
    3840x2160: the cells of the 23 exact twins read 0; `ciede`, a bounded
    cell (1e-9, ADR-1436), reads 1.1e-12 at 3840x2160 (3.5e-12 with the
    build before this change). The CPU half of that build equals the GCC
    build on 10126 of 10126 values of those runs, and the twins equal the
    GCC build's CPU extractor on 9987 of 10004; the 17 others are `ciede` at
    3840x2160. `test_sycl_exact_twins`, `test_sycl_ms_ssim_parity`,
    `test_sycl_float_psnr_parity` and `test_sycl_ciede_parity` pass.
  - The published images pick this up when they are rebuilt; the
    Containerfile needs no change. The `Linux Intel LLVM` job runs the whole
    `meson test` suite, so it runs the new test; the two SYCL lanes of
    `libvmaf-build-matrix.yml` run it as a step of their own.
  - Windows `icx-cl` (`intel-llvm-cl`) links another set of libraries and is
    not covered: no Windows host was available to measure it
    (`T-ICX-CL-WINDOWS-HOST-MATH-2026-10-03`).
  - The golden gate's build profile (ADR-1317) still refuses icx builds; this
    change removes one of the reasons, not the decision.
  - `testdata/scores_sycl_a380_720.json` holds frame 26's `vmaf` from an icx
    build with libimf (88.435637 against the CPU snapshot's 88.435634). The
    same recording (`--backend sycl`, `vmaf_v0.6.1`, A380) from the host SYCL
    build with this change equals `testdata/scores_cpu_720.json` on 720 of
    720 values, from the build before it on 719. The file is re-recorded in
    the rebuilt image, not here (ADR-1102; `T-SYCL-SNAPSHOTS-STALE-2026-10-02`).

## References

- `Q` (maintainer popup, 2026-10-03, on `T-ICX-LIBIMF-HOST-MATH-2026-10-01`):
  "Fix in RC3 (Recommended)".
- `/opt/intel/oneapi/compiler/2026.0/share/man/man1/icx.1` (`-no-intel-lib`,
  `-static-intel`, `-shared-intel`, `-fimf-use-svml`); `icx --help` of
  oneAPI 2026.0.0 and 2026.1.1.
- [ADR-1461](1461-strict-fp-every-translation-unit.md) (the strict FP project
  argument), [ADR-1415](1415-x86-simd-libraries-strict-fp.md),
  [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) (the SYCL device FP
  policy), [ADR-1475](1475-integer-adm-quant-step-upstream-float.md) (the
  integer ADM frame), [ADR-1434](1434-sycl-float-adm-cpu-arithmetic.md),
  [ADR-1317](1317-golden-gate-build-isolation.md).
