<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Build, FP, tidy, and parity: `core/src/feature/sycl/` agent invariants

core/meson.build, core/meson_options.txt, core/src/meson.build, sycl_compat.h,
sycl_exact_fp.h, sycl_soft_double.h, sycl_soft_signed.h, .clang-tidy,
scripts/ci/clang-tidy-sycl.sh, scripts/ci/gen-sycl-compile-commands.py,
scripts/ci/exact_twins.d/*, scripts/ci/cross_backend*.py,
core/test/test_sycl_exact_twins.c, core/test/test_sycl*parity.c; strict-FP,
soft-fp64, orientation.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `/core/meson_options.txt`, `/core/meson.build`, `/core/src/meson.build` | [build](AGENTS.d/build.md) | SYCL feature TUs compile only when meson setup -Denable_sycl=true. |
| `/scripts/ci/clang-tidy-sycl.sh`, `/scripts/ci/gen-sycl-compile-commands.py`, `/.clang-tidy` | [clang-tidy](AGENTS.d/clang-tidy.md) | icpx-aware clang-tidy; stock LLVM clang-tidy cannot resolve <sycl/sycl.hpp>. |
| `/scripts/ci/exact_twins.d/*`, `/scripts/ci/cross_backend_calibration.py`, `/core/test/test_sycl_exact_twins.c` | [exact-twins](AGENTS.d/exact-twins.md) | Twins declared exact as group; bit-identical with CPU reference, measured at --precision max. |
| `sycl_soft_double.h`, `sycl_soft_signed.h` | [fp64-fallback](AGENTS.d/fp64-fallback.md) | fp64-free kernels non-negotiable; double emulation through soft double on hardware without native fp64. |
| `/core/src/meson.build`, `sycl_compat.h` | [orientation](AGENTS.d/orientation.md) | Orientation for agents on per-feature SYCL kernels and governing ADRs. |
| `/scripts/ci/cross_backend_parity_gate.py`, `/core/test/test_sycl_*_parity.c` | [parity-tests](AGENTS.d/parity-tests.md) | Every shipping SYCL kernel here must have scalar-vs-SYCL parity test. |
| `sycl_exact_fp.h`, `/core/src/meson.build` | [strict-fp](AGENTS.d/strict-fp.md) | Every TU is strict-clean; SYCL strict FP line load-bearing, one line for every TU. |
