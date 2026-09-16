<!-- markdownlint-disable MD013 -->

# 2060 — Three gates that only fail in combination

**Date**: 2026-09-16
**Scope**: the `Cppcheck`, `Tidy Ratchet` and `Linux Intel LLVM` failures on the
rc.1 integration train (#1425).
**Outcome**: eight cited cppcheck suppressions, four clang-tidy fixes, a
re-measured ratchet baseline, and the close-out of
`T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07`.

## Why a train finds things its branches could not

Every pull request folded into the train was green on its own. Three of the
failures below exist only because two independently-correct changes met.

## 1. `--library=posix` plus a POSIX idiom

One folded branch taught the Cppcheck gate to load cppcheck's POSIX model
(`--library=posix --check-level=exhaustive`). Other branches, and master before
them, spell the descriptor handoff the way POSIX requires:

```c
FILE *fp = fdopen(fd, "w");
if (!fp) {
    (void)close(fd);      /* fdopen failed: the fd is still ours */
    return NULL;
}
```

cppcheck's `posix.cfg` declares `<dealloc>fdopen</dealloc>` for the descriptor
resource — unconditionally, with no notion of the call failing. Version 2.13.0
therefore reads the `close()` as a second free and reports `doubleFree` at eight
sites. 2.21.1 does not. CI installs 24.04's apt cppcheck, which is 2.13.0.

Measured both ways:

```console
$ cppcheck --library=posix --check-level=exhaustive core/tools/vmaf.cpp   # 2.21.1
(nothing)
$ docker run --rm -v "$PWD:/w:ro" -w /w ubuntu:24.04 \
    bash -lc 'apt-get install -y cppcheck && cppcheck --library=posix \
      --check-level=exhaustive core/tools/vmaf.cpp'                       # 2.13.0
core/tools/vmaf.cpp:156:15: error: Resource handle 'raw_fd' freed twice. [doubleFree]
```

Each site now carries a cited `cppcheck-suppress doubleFree` with the reason.
Re-running 2.13.0 over the six files with the gate's real compile flags exits 0.

**Worth remembering**: the three sites where the `if` had no braces needed them
adding. A comment between an unbraced `if` and its statement makes clang-tidy's
`readability-braces-around-statements` fire, so the suppression that silenced
cppcheck introduced a clang-tidy warning. The two gates have to be run together.

## 2. A ratchet baseline measured by a compiler the lane no longer uses

Another folded branch moved the lint lane from gcc-14 to gcc-15, on the correct
reasoning that gcc supplies the system headers clang-tidy parses and a lane
pinned a major behind every developer's box makes a ratchet delta
unreproducible. What it did not do is re-measure the baseline in the same
change. The committed baseline says 1,229 warnings over 292 translation units;
the lane measures 1,059 over 306. Twenty-five files improved, and
`core/test/test_thread_pool.c` moved 5 → 8 although the train does not touch it.

Five files were genuine regressions and were fixed rather than baselined:

| File | Was | Now | What it was |
| --- | --- | --- | --- |
| `ssimulacra2_avx2.c` | 3 | 4 | the ADR-1208 edge-diff rewrite pushed the function to 65 lines against a 60 threshold |
| `ssimulacra2_avx512.c` | 3 | 4 | same |
| `test_feature_isa_invariance.c` | 0 | 8 | a new file: seven `NULL`s and a `memcmp` over two doubles |
| `test_fex_ctx_vector.cpp` | 0 | 1 | see below |
| `test_thread_pool.c` | 5 | 8 | not a regression — engine drift, absorbed by the re-measurement |

The two SIMD files were fixed by extracting the ten lines the vector body and
the scalar tail each carried into one `edge_diff_accum_d` helper. That removes
the duplication ADR-1208 exists to prevent and takes both functions to about 40
lines. Instruction histograms before and after are identical for AVX-512 and
differ for AVX2 only in `mov`/`nop`/`vxorpd` counts — register allocation and
padding, no arithmetic — and `test_ssimulacra2_simd` and
`test_feature_isa_invariance` both still pass.

`test_fex_ctx_vector.cpp` is the more instructive one. Adding
`VMAF_WRAP_EXPORT` to the `__wrap_*` signature pushed it past 100 columns, so
clang-format broke after the return type and the identifier landed two lines
below its next-line suppression marker, which reaches exactly one line. A
begin/end band replaces it, because a band survives reflow.

**Reproducing the lane's numbers locally matters more than it looks.** A local
clang-tidy 22.1.8 against gcc-16 headers agrees with the lane on most files and
disagrees on two: `dict.cpp` (15 vs 16) and `feature_collector.cpp` (13 vs 16),
both `cert-dcl03-c,misc-static-assert`, which depends on how `assert` expands in
the system header. A baseline written from that measurement would have failed
the gate again. The re-measurement therefore runs in a container built to the
lane's own recipe — gcc-15 from `ppa:ubuntu-toolchain-r/test`, clang-tidy-22
from apt.llvm.org, meson from PyPI — and reproduces the lane's per-file counts
exactly. `xxd` has to be in that container or the eighteen embedded-model
translation units are never generated and the count comes out 18 TUs short.

## 3. The flag order that had been re-enabling what it asked to disable

`T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07` recorded that `-fp-model=precise`
implies `-ffp-contract=on`, so spelling the pair as
`['-ffp-contract=off'] + _x86_simd_strict_fp_extra` re-enables contraction under
icx — the opposite of the intent. Nine carve-outs were affected. The row also
recorded that reordering them is not safe, because it had been tried and it
broke `test_ssimulacra2_simd` with `picture_to_linear_rgb SIMD not bit-identical
to scalar`.

The missing half is that `core/test/meson.build` builds `_simd_strict_fp_args`
with the same mis-ordering, and the SIMD tests compile their own copies of the
scalar references with it. Reordering only the kernels moves one side of every
comparison. Reordering both moves them together:

```console
$ CC=icx CXX=icpx meson setup bicx core --buildtype release -Db_lto=false \
    -Denable_cuda=false -Denable_sycl=false -Denable_float=true
$ ninja -C bicx && ./bicx/test/test_feature_isa_invariance
ISA invariance FAIL ssimulacra2: host-isa=-38.37695186087862 scalar=-38.376932759633718
# after reordering both files:
1 tests run, 1 passed
$ meson test -C bicx
Ok: 150   Fail: 0
```

The scalar figure, `-38.376932759633718`, is what every other lane produces, so
contraction had been moving the SIMD side.

Twelve carve-outs now spell the pair `_x86_simd_strict_fp_extra +
['-ffp-contract=off']`: six AVX2, three AVX-512 and the three scalar-reference
libraries (`psnr_hvs`, `ssimulacra2`, `y_funque_plus`). On gcc and clang both
lists are empty, so the reorder is a no-op there, and the gcc suite stays green
at 148 tests.

## What the ADR-1207 gate is worth

Three of the six defects the train's first full matrix produced came from this
one test, and each was a pre-existing violation of the ADR-0891 bit-exactness
contract that every existing SIMD test missed. Those tests compare a SIMD kernel
against a reference defined in the test translation unit; when a flag or a
policy moves, both sides move together and the comparison stays true while the
shipped scalar path drifts away from both. Driving the public API twice over one
fixture has no such blind spot.

Its fourth find took the longest and is the best argument for it. On Windows
the shipped scalar `ssimulacra2` disagreed with its own SIMD twins by 0.37
points and `float_ms_ssim` by 2.4e-7 — on master, invisible to every existing
test, because `test_ssimulacra2_simd` compares the SIMD kernels against
references defined in the test translation unit and those agreed with each
other perfectly.

Four hypotheses died on measurement before the right one: FP contraction (forcing
it on in the scalar TU on Linux moves the score the *other* way and by a tenth as
much), `sqrtf` accuracy (msvcrt's matches the hardware instruction exactly over
the sampled range), `-fno-math-errno` (no effect), and my own first
"reproduction", which turned out to be a broken-git artefact.

What found it was building the thing. Arch's `mingw-w64-gcc` is 16.2.0 — the
runner's exact compiler — and cross-building with it in a container reproduced
**nothing**: scalar and SIMD agreed bit for bit. That null result was the clue.
Arch's toolchain is UCRT-based; MSYS2's `MINGW64` links the legacy `msvcrt.dll`.
Rebuilding with Fedora's msvcrt-based `mingw64-gcc` reproduced CI's numbers to
the last digit, which turned the question from "what is wrong with Windows" into
"what does msvcrt do differently".

Bisecting then took one pass. A temporary knob forcing exactly one of
ssimulacra2's seven dispatch slots back to scalar, run seven times under wine,
put the whole 0.37 on slot 6 — `picture_to_linear_rgb` — and the mechanism was
sitting in a comment there: `fmaf()`, chosen by ADR-0891 precisely because it is
a *single-rounded* fused multiply-add. It is one on glibc, musl and the UCRT. On
legacy msvcrt it is not, so the scalar path rounded twice while the SIMD path
rounded once, and ADR-1205's warning — that one ULP here becomes a 2.6e-3 score
delta because the pipeline is ill-conditioned downstream — came true at 0.37.
`ms_ssim_decimate.c` had the same two calls, which is why exactly two features
failed and not one.

The fix is `vmaf_fmaf_exact()`: evaluate in `double`, round once. For binary32
operands the product is exact in binary64 and binary64's 53 bits clear the
2p + 2 = 50 that makes the final rounding innocuous, so it is the
correctly-rounded fused result on every host without FMA hardware or a libm
call. Measured against glibc `fmaf` and against `vfmadd213ss` over 20 million
random triples: zero differences. Every Linux pooled score is unchanged to the
last bit, and on the msvcrt build scalar and SIMD now agree exactly, both equal
to Linux. See [ADR-1253](../adr/1253-scalar-fma-not-fused-on-msvcrt.md).
