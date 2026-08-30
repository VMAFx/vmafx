- CI: every workflow now installs `meson` from PyPI instead of `apt`.
  Ubuntu 24.04 ships meson 1.3.2, which predates `c23` in `c_std`
  (verified: 1.3.2 rejects it, 1.4.0 accepts it), so every job that
  configured `core/` failed at `meson setup` with
  `ERROR: Unknown C std ['c23']` from the moment ADR-0692 raised the
  standard. This took out Rust, Sanitizers, Tests & Quality Gates
  (including the Netflix Golden leg), Lint, Go, Security Scans and
  Supply Chain — 15 install sites across 7 workflows. The workflows
  that were already green (`build.yml`, `fuzz.yml`,
  `ffmpeg-integration.yml`, `libvmaf-build-matrix.yml`) had always
  pip-installed meson; the fix brings the remaining seven in line
  rather than inventing a new pattern.
- `core/meson.build` now declares `meson_version: '>= 1.4.0'` (was
  `'>= 0.58.0'`). The project has de-facto required 1.4.0 since it
  adopted `c_std=c23`; declaring it turns the cryptic
  `Unknown C std ['c23']` into
  `ERROR: Meson version is 1.3.2 but project requires >= 1.4.0`.
- `core/meson.build` no longer puts `c_std` in `default_options` at all;
  the C standard is now selected by compiler identity after `project()`,
  mirroring the `cpp_std` handling ADR-1056 already added for C++. The accepted-values list meson advertises for
  `c_std` is compiler-specific, so *any* value in `default_options`
  aborts configure on a toolchain missing that exact spelling — before
  any conditional in the file can run. Observed on CI:
  MSVC offers only `['none','c89','c99','c11']`, and GCC 13 (the
  `ubuntu-latest` default) offers `c2x` but not `c23`. A bare `c23`
  therefore failed with
  `None of values ['c23'] are supported by the C compiler` — a
  *different* failure from the stale-meson one above, and the one that
  actually broke the required `Build — Ubuntu gcc (CPU) + DNN` and both
  Windows MSVC legs even where meson was new enough. The selection is
  now `/std:clatest` on MSVC, `-std=c23` where the compiler accepts it,
  and `-std=c2x` otherwise — the pre-ratification spelling of the same
  standard, so this is a spelling fallback, not a language downgrade.
  An explicit `-Dc_std=` override is still honoured untouched.
- `core/meson.build` also stopped hard-coding `-std=c++26`. GCC 13 — the
  `ubuntu-latest` default — rejects that flag outright
  (`unrecognized command-line option '-std=c++26'`), so every C++ TU
  failed to build. This was invisible for as long as the `c23` defects
  above aborted configure before any C++ source was reached; fixing them
  exposed it. The project-level flag is now the newest of
  `c++26` / `c++23` / `c++2b` that the compiler actually accepts, with a
  hard `error()` if none do. Per-target `override_options` in
  `core/src/meson.build` are untouched.
- The C++ standard probe compile-tests `<expected>` rather than trusting
  `-std=` flag acceptance. clang 18.1.3 *accepts* `-std=c++26` but its
  accompanying libstdc++ then has no `std::expected`, so
  `core/src/dict.cpp` failed with
  `no member named 'expected' in namespace 'std'` and took out all four
  Sanitizers checks. ADR-0692 (#1140) raised the tree from `c++23` to
  `c++26` earlier the same day; `<expected>` is a C++23 feature and the
  only non-C++17 header in the tree besides `<span>` (C++20), so C++26
  buys nothing here while breaking that toolchain. The probe now takes
  the newest candidate that genuinely builds — c++26 where it works,
  c++23 where it does not.
