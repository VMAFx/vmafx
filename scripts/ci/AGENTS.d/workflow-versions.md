---
paths:
  - scripts/ci/check-workflow-versions.py
  - scripts/ci/tests/test_level_zero_single_source.py
  - scripts/ci/tests/test_formatter_pins_single_source.py
  - scripts/ci/tests/test_mypy_python_version_single_source.py
invariant: Level Zero, ruff / black and mypy `python_version`: one owner each plus checked mirrors; raise paired pins together.
---
<!-- markdownlint-disable MD013 MD060 -->
# Version pins checked by `check-workflow-versions.py`

## Level Zero version consumption (ADR-1231)

`dev/Containerfile` copies and sources `build-config.env` in its SDK download
RUN. Loader version = runtime shell input, so needs no Docker ARG
mirror. Preserve source step, both URL components and command-execution
fixture in `tests/test_level_zero_single_source.py`; base-image test hook
runs both single-source suites. `check-workflow-versions.py` verifies this
container consumer alongside Windows workflow mirror. Renovate tracks
Level Zero only in `build-config.env`; ROCm uses central image manager.

`check-workflow-versions.py` also owns formatter pins: `Makefile`'s
`RUFF_VERSION` / `BLACK_VERSION` must equal ruff-pre-commit and black revs
in `.pre-commit-config.yaml`, and recipe may not spell `ruff==<n>` or
`black==<n>` as literal. Renovate moves both files through
`pre-commit hooks` group (two regex managers on `Makefile`); keep group
name identical on both rules or bumps split into two pull requests and
first one fails this gate. Fixture: `tests/test_formatter_pins_single_source.py`.
same checker keeps `VIRTUAL_ENV_PATH` absolute and rejects recipe-local
`$(VENV)/bin` prefixes: Meson persists Ninja path and invokes it from
build directory while generating `compile_commands.json`, so relative path
breaks canonical `make lint` gate. Cython likewise invokes absolute
`$(VENV_PYTHON)` after changing into `python/`.

`check_mypy_python_version` owns third pairing (ADR-1282): `pyproject.toml`'s
`[tool.mypy] python_version` must equal `major.minor` floor of
`[project] requires-python` and `major.minor` of `PYTHON_CI_VERSION`.
Deleting key is finding too — mypy would then model whichever interpreter
caller runs. comment was only thing holding these together before, and
pin stayed at `3.10` against `>=3.14` floor long enough to abort every
`ai/src/` run (mypy will not parse numpy's PEP 695 `type` statement below 3.12).
Raise all three in one commit. Fixture:
`tests/test_mypy_python_version_single_source.py`; add its path and any new
trigger file to `test-base-image-single-source` hook's `files:` regex, which
is what decides when `test_*single_source.py` discovery runs.
