- The libFuzzer harnesses configure and build again. `core/test/fuzz/meson.build`
  still listed `../../src/dict.c`, which became `dict.cpp` in the C++23 rewrite
  (#1054) — the same rename-fallout class as the Cython `mem.c` break fixed
  earlier today. Every fuzz job (`Nightly Fuzz — libFuzzer` and the
  `fuzz-nightly` job in `Sanitizers`) had failed at `meson setup` with
  `ERROR: File ../../src/dict.c does not exist` since 2026-06-27 — unnoticed
  because they only run nightly. The target now names `dict.cpp` and carries
  `cpp_args : fuzz_flags` so the parser under test keeps libFuzzer coverage
  instrumentation. Verified: a full `-Dfuzz=true -Db_sanitize=address` build
  with clang links all four harnesses.
- `scripts/setup/ubuntu.sh` installs meson from PyPI instead of apt. The
  `nightly` workflow's TSan, benchmark and full-clang-tidy jobs all bootstrap
  through this script, so they kept getting Ubuntu's meson 1.3.2 after the
  15-site workflow sweep (#1161) and failed with
  `Meson version is 1.3.2 but project requires >= 1.4.0`. It also drops
  `isort` from the linter install, retired in ADR-1126.
