- **CI's gcc stops being the one toolchain component frozen at the distro's
  version** (ADR-1230). The clang-tidy lanes already install clang-22 from
  `apt.llvm.org` and meson from PyPI, both because `ubuntu-24.04`'s versions
  cannot handle the tree's C23 / C++26 target — but gcc was left at the image's
  gcc-14 with no rationale anywhere. It now comes from
  `ppa:ubuntu-toolchain-r/test` at gcc-15, the same treatment for the same
  reason.
- **The clang-tidy ratchet is diagnosable.** It records `cc_version` alongside
  `clang_tidy_version` — clang-tidy parses every translation unit against the
  system headers the *C compiler* supplies, so the warning counts depend on gcc
  as much as on clang-tidy, and recording only one of the two made a count that
  disagreed with local measurement impossible to explain. A compiler mismatch
  against the baseline now annotates a warning, exactly as a clang-tidy
  mismatch already did. The `--report` artifact additionally keeps every
  diagnostic as `path:line:col: [check]`; the script already parsed them and
  threw them away, leaving "which warning?" unanswered anywhere in CI's output.
  The baseline deliberately stays counts-only so it remains reviewable and does
  not churn on line-number shifts.
- **Requires a CI-side baseline regeneration**: warning counts under gcc-15's
  headers do not equal gcc-14's, so `scripts/ci/tidy-baseline-cpu.json` must be
  rewritten from a CI run rather than a developer machine.
