---
paths:
  - core/src/meson.build
  - core/test/meson.build
  - dev/Containerfile
invariant: CC=gcc CXX=icpx builds give icpx C++ the strict spelling and link SYCL-on tests as C++.
---
<!-- markdownlint-disable MD013 MD060 -->
# Hybrid GCC C + icpx C++ / SYCL toolchain (ADR-1714)

## Rebase-sensitive invariants

- **C++ strict spelling under a mixed toolchain (ADR-1714)**: ADR-1461's
  policy block keys on `cc.get_id()`. With `CC=gcc CXX=icpx` (the dev
  container and `Containerfile.vmafx`) C++ would get GCC's
  `-ffp-contract=off` only and stay in icpx's default fast model. The
  `BEGIN/END VMAF C++ strict FP policy for mixed toolchains` block right after
  the ADR-1461 project argument adds `-fp-model=precise -ffp-contract=off` as
  a project argument for `cpp` when `cxx.get_id() == 'intel-llvm'` and the C
  compiler is not Intel LLVM. On rebase: keep it above the first target and
  keep contraction-off last; never put `-fp-model=precise` on one target
  (after the project argument it re-enables contraction, ADR-1461).
  `test_strict_fp_compiler_args` executes the block for the compiler pairs and
  checks every compile command by its own language's compiler.
- **No libimf option in the Containerfiles**: ADR-1495 already gives every
  Intel LLVM link `-no-intel-lib=libimf` per language; the containers pass
  only `CC=gcc CXX=icpx` and `-Db_lto=false` (GCC LTO objects cannot go
  through the icpx link).
- **`test_link_kwargs`**: see `core/test/AGENTS.d/hybrid-toolchain-test-link.md`.
