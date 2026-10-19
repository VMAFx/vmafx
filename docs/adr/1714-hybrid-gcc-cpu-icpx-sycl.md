<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1714: The dev and SYCL+ffmpeg containers build CPU C code with GCC and SYCL with icpx

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `build`, `container`, `sycl`, `numerics`, `performance`, `fork-local`

## Context

The dev container (`dev/Containerfile`) and the SYCL + ffmpeg container
(`Containerfile.vmafx`, [ADR-1715](1715-vmafx-sycl-ffmpeg-container.md)) build
libvmaf with `CC=icx CXX=icpx`, because the SYCL kernels need the oneAPI C++
compiler. That also puts every CPU C translation unit under `icx`.

When this decision was first written (on master `42fb501cc`), an icx build
returned other CPU scores than a GCC build: Intel's math library (`libimf`)
was linked instead of glibc `libm`, and libsvm (`svm.cpp`) ran under icpx's
default `-fp-model=fast`. Master has since fixed both for every icx build:
[ADR-1461](1461-strict-fp-every-translation-unit.md) puts the strict FP
policy on every C and C++ translation unit as a project argument, and
[ADR-1495](1495-icx-system-libm.md) passes `-no-intel-lib=libimf` to every
Intel LLVM link, per language. This ADR keeps only what those two do not
cover:

- **Speed.** On the old base the GCC-compiled CPU path was 8-12 % faster
  single-threaded at 1080p and 2.7 % faster with 16 threads, SYCL unchanged.
  On master `b01ffe42d` (strict model on every translation unit, glibc math)
  a 1080p re-measurement (60 frames, 9 interleaved rounds, 1-3 % round-to-round
  spread, research digest) still shows it single-threaded: `vmaf_v0.6.1`
  -4.9 %, `vmaf_float_v0.6.1` -10.9 %, `cambi` -6.5 % against icx; SpEED
  -1.1 %, 16 threads -2.6 % and SYCL -0.6 % are within noise. The hybrid
  equals a pure GCC build in every CPU configuration (at most 0.4 points), so
  the gain is GCC compiling the CPU C code, and the hybrid keeps SYCL.
- **The mixed toolchain itself.** With `CC=gcc CXX=icpx`, ADR-1461's policy
  block keys on the C compiler (`cc.get_id()`), so the C++ translation units
  icpx compiles got GCC's spelling, `-ffp-contract=off`, and stayed in icpx's
  default fast model (reassociation, approximate division). And
  `sycl_dependency` carries `-fsycl` and the AOT target flags in its link
  arguments (ADR-1099, ADR-1360); only the icpx driver accepts them, so a
  C-only test executable linked by GCC failed to link.

## Decision

Both containers configure libvmaf with `CC=gcc CXX=icpx` (lld linker
unchanged) and `-Db_lto=false` (GCC LTO objects cannot go through the icpx
link). The libimf part needs no option of its own any more: ADR-1495 already
gives every C++ link under icpx `-no-intel-lib=libimf`, and the C links are
GCC's. Two Meson changes make the mixed toolchain work and keep it strict:

- `core/src/meson.build`, next to the ADR-1461 policy block: when the C++
  compiler is Intel LLVM and the C compiler is not, every C++ translation
  unit also takes icpx's strict spelling (`-fp-model=precise
  -ffp-contract=off`, the same list an icx build gives it) as a project
  argument for `cpp`. This replaces the `-fp-model=precise` the first
  version of this ADR put on libsvm alone, which ADR-1461 forbids (after the
  project argument it turns contraction back on).
- `core/test/meson.build`: with SYCL on, every test executable links as C++
  (`test_link_kwargs = {'link_language' : 'cpp'}`).

CI and the release images keep `CC=icx`; the pure-icx configuration stays
covered there.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **GCC C + icpx C++, the C++ strict spelling for icpx (chosen)** | Faster CPU path; C++ under icpx as strict as in an icx build; SYCL unchanged | Two compilers in one build; LTO off; CUDA/HIP host C in the dev image is GCC-built | — |
| Keep `icx` everywhere | One compiler; LTO on; since ADR-1461 and ADR-1495 the same scores as GCC | CPU path 5-11 % slower single-threaded at 1080p (re-measured on `b01ffe42d`) | Speed |
| Hybrid with `-fp-model=precise` on libsvm only (first version of this ADR) | One target | Other C++ translation units stay in icpx's fast model; after ADR-1461's project argument the flag re-enables contraction in `svm.cpp`, which `test_strict_fp_compiler_args` rejects | Wrong scope, now also wrong order |
| Hybrid with ADR-1461's block keyed on the C++ compiler as well | One block | The block's variables feed C-only targets and the CUDA host flags; splitting them per language changes every consumer | Larger change for the same effect |
| Hybrid with `b_lto=true` | Whole-program LTO | GCC GIMPLE LTO objects cannot be read by the icpx/lld link | Does not link |

## Consequences

- **Positive**: container CPU scores equal a GCC build: after the rebase
  onto `b01ffe42d`, 2550 of 2550 values bit for bit at each dispatch (`src01`
  pair and the 1-pixel checkerboard pair, 14 extractors plus the default
  model, `--precision max`, scalar and default dispatch), as an icx build's
  now do too; `libvmaf.so` links glibc `libm` only. A mixed toolchain builds
  and its tests link, and its C++ units are as strict as an icx build's.
- **Negative**: no LTO in these container builds. The dev image's CUDA and
  HIP host C is GCC-compiled; there is no NVIDIA or AMD GPU on the
  measurement host, so that path is build-verified only. With the numerical
  reasons gone (ADR-1461, ADR-1495) the toolchain choice rests on speed: a
  short single-threaded run on the 576x324 pair after the rebase showed no
  clear gain, and the 1080p re-measurement above (bit-identical outputs
  across icx, hybrid and GCC) shows 5-11 % on the heavy single-threaded
  configurations and noise elsewhere.
- **Neutral / follow-ups**: every new test executable must pass
  `kwargs : test_link_kwargs` unless it sets `link_language` itself. The
  release images (`docker/Dockerfile.production-gpu`) could adopt the same
  toolchain in a later decision.

## References

- req: "да, применяй V3 и запускай V4"; 2026-10-01 popups: hybrid in both
  containers, `-no-intel-lib=libimf` after measurement, SYCL validated on the
  NAS Arc A380, port onto a new branch from master.
- Research digest: [`docs/research/1714-hybrid-gcc-cpu-icpx-sycl.md`](../research/1714-hybrid-gcc-cpu-icpx-sycl.md).
- [ADR-1461](1461-strict-fp-every-translation-unit.md), [ADR-1495](1495-icx-system-libm.md),
  [ADR-1415](1415-x86-simd-libraries-strict-fp.md), [ADR-1099](1099-sycl-fsycl-link-propagation.md),
  [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md), [ADR-1715](1715-vmafx-sycl-ffmpeg-container.md).
- Originally drafted as ADR-1440; renumbered to 1502 on the rebase onto
  master `b01ffe42d`, where 1440 had been taken, and to 1561 on the
  rebase onto master `2889f963a`, where 1502 had been taken; then to 1593 on the
  rebase onto master `f224b1b42`, where 1561 had been claimed by other branches
  (and 1562 taken by master).
- Renumbered to 1714 when it was ported to `VMAFx/vmafx` master `dc9cd9481`,
  where 1593 had been taken and numbers up to 1713 had been claimed by
  `VMAFx/vmafx` branches.
