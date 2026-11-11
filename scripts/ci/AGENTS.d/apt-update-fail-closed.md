---
paths:
  - scripts/ci/test_fail_closed_ci.py
  - scripts/ci/apt-install-cached.sh
  - scripts/ci/install-*.sh
  - scripts/dev/tidy-lane.sh
  - .pre-commit-config.yaml
invariant: ADR-3537 requires --error-on=any and -o Acquire::Retries=3 on every tracked apt-get update physical line.
area: gates
---
# APT index refresh fail-closed contract (ADR-3537)

`AptUpdateFailClosedContract` in `scripts/ci/test_fail_closed_ci.py` enforces
[ADR-3537](../../../docs/adr/3537-apt-update-fail-closed.md) via
`apt_update_problems(path, text)` and `apt_update_contract_problems(root)`.

Scope = `ci_text_paths(root)` (`git ls-files -z`: `.yml`, `.yaml`, `.sh`,
`.bash` suffixes; `Dockerfile*` and `Containerfile*` basenames; 1,864-file
bound). Reuses `fail-closed-ci-contract` hook in `.pre-commit-config.yaml` and
`DisposableRepoTestCase` fixture isolation; no second path lister.

Allow-list text rule, zero shell or YAML parsing:

- Physical line matching `\bapt(?:-get)?\b[^\n]*\bupdate\b` (case-insensitive)
  passes only when non-comment lowercase `apt-get` call carries both
  `--error-on=any` and `-o Acquire::Retries=<N>` (`N >= 1`) on every `update`
  clause on line.
- Bare `apt update`, mixed-case `APT-GET UPDATE`, trailing-comment flags and
  comment mentions fail closed; reword comments to `apt index refresh` or `apt
  list refresh`.
- Diagnostic format:

```text
<path>:<line>: apt-get update must fail on index errors: add --error-on=any -o Acquire::Retries=3 (ADR-3537)
```

Fixtures: positive (Dockerfile, `.sh`, workflow `run:`, missing retries,
missing `--error-on=any`, bare `apt`, mixed case, comments), negative (flags
before or after `update`, `apt-get install`, `.py`/`.md`, untracked, empty),
boundary (two `update` calls on one line), planted
(`scripts/ci/install-cuda-toolkit.sh` missing `--error-on=any`).
