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
