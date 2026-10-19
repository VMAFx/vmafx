## `build.yml` plans on `build_work`, not `c_core` (2026-10-13)

- `.github/ci-impact.json` gains the selector `build_work`: `c_core` plus
  `python/**`, `compat/**`, the build, pytest-timeout and tox lock files and
  `scripts/ci/prune-corrupt-fixtures.sh`. `.github/workflows/build.yml` reads it
  in the `impact` job.
- **On sync**: fork-only files; no upstream counterpart. A new `-r` lock,
  `tox -c` config or `python -m pytest` target in `build.yml` adds its path;
  `BuildWorkRouting` in `scripts/ci/tests/test_ci_impact.py` fails otherwise.
  Against master's config (`build.yml` on `c_core`) the test file fails 26 tests.
