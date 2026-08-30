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
- `core/meson.build` now asks for `c_std=c23,c2x` rather than a bare
  `c23`. Meson walks the candidate list and takes the first value the C
  compiler actually supports, so GCC 14+ / Clang 18+ still get `c23`
  while GCC 13 — the default on `ubuntu-latest` runners — takes the
  older `c2x` spelling of the same standard. A bare `c23` aborted
  configure on those runners with
  `None of values ['c23'] are supported by the C compiler`, which is a
  *different* failure from the stale-meson one above and hit the build
  matrix legs (including the required `Build — Ubuntu gcc (CPU) + DNN`)
  even where meson was new enough.
