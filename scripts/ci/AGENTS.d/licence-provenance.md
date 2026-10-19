---
paths:
  - scripts/ci/upstream_parity_pin.py
  - scripts/ci/tests/test_upstream_parity_pin.py
  - scripts/ci/check_licence_metadata.py
  - scripts/ci/tests/test_check_licence_metadata.py
invariant: Parity heading pins upstream; root, manifest licences match shipped files.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Licence provenance job and recorded upstream head

## Workflow coupling

| Script | Invoked by | Coupling |
| --- | --- | --- |
| `upstream_parity_pin.py` | `lint-and-format.yml` — `licence-provenance` job, check name `Licence Provenance` ([ADR-1474](../../../docs/adr/1474-relicense-helper-headers-and-ci-check.md)); `test-upstream-parity-pin` pre-commit hook | Reads one heading of `docs/development/known-upstream-bugs.md`: ``## Upstream head the fork is at parity with: `<commit id>` (<date>)``. Job passes resolved id to `scripts/dev/relicense_fork_files.py --check --upstream-ref`. Zero headings, two headings, reworded heading, id outside Netflix `master` -> exit 2, job red. |

## Invariants

- Pin = recorded parity head, never upstream moving `master`: Netflix push never
  turns unrelated pull request red. Upstream port or sync moves id in same
  pull request.
- Job needs full history both sides: `fetch-depth: 0`, unshallow fetch of
  Netflix `master`. `relicense_fork_files.py` refuses shallow checkout
  (`require_full_history()`); keep that guard.
- `Licence Provenance` sits in aggregator `required` + `strictMustReport` and in
  `ADR_1474_STRICT_CONTEXTS` (`tests/test_hiss_replay_contract.py`). Rename in
  all places, same commit. No path filter, no `continue-on-error`.
- `tests/test_upstream_parity_pin.py` pins heading grammar, resolution and job
  text. Change parser, heading form or job steps -> change test, same commit.
- Human guide: `docs/development/licence-provenance-check.md`.

## Root licence files and package licence fields (ADR-1699)

`check_licence_metadata.py` = one gate, two rules, stdlib only (setuptools
replaced by stand-in module for `python/setup.py` probe).

- Root: `LICENSE` byte for byte `LICENSES/EUPL-1.2.txt`;
  `NOTICE` keeps both
  `SPDX short identifier: BSD-2-Clause-Patent` and
  `Copyright (c) 2020 Netflix, Inc.`; `NOTICE` missing -> refused; no other
  root name licensee 10.1.0 scores (`FILENAME_REGEXES`: `LICENSE*`, `COPYING*`,
  `COPYRIGHT*`, `OFL*`, `PATENTS*`, `<x>-LICENSE*`) -> `LICENSE` only root
  licence file (licensee 9.18.0: `EUPL-1.2`; >= 9.19 also reads `LICENSES/`); `licensing.json` `spdx_texts` names both. Upstream sync
  touching Netflix `LICENSE` -> change lands in `NOTICE`.
- Manifests: `pyproject.toml`, `Cargo.toml`, `Chart.yaml`, `package.json`
  with licence field -> AND of exactly shipped files' SPDX ids (licensing.py
  reader: header, else `REUSE.toml`). OR refused. Unmodelled selection
  (`include`, `exclude`, `license-file`, `.helmignore`, npm `files`, hatch sdist
  keys) -> exit 2, model it; never guess.
- Cargo model = `cargo package --list` + workspace manifest of inherited fields:
  tracked crate files outside nested packages, workspace `Cargo.lock`, readme
  copied from outside unless crate has same-name file. Cargo changes this ->
  change model + `CargoTests`, same commit.
- Helm: chart's own files only; each `charts/*.tgz` own package, its
  `Chart.yaml` declaration = its `REUSE.toml` record.
- Fixtures and gate drop caller `GIT_*` for non-repo roots: hook
  `GIT_INDEX_FILE` / `GIT_DIR` once wrote fixture entries into worktree index
  and set `core.bare=true` on shared config (`FixtureIsolationTests`).
- Python model (ADR-1560) lives here; `python/test/setup_metadata_test.py`
  imports it. One implementation.
- Wiring: required `Licence Provenance` job step; pre-commit
  `check-licence-metadata` (`always_run`, skipped in CI pre-commit job:
  job owns it, ADR-1568); tests in tooling suite, which root `LICENSE*`,
  `LICENSES/`, `REUSE.toml` select. `WiringTests` pin job step + hook.
