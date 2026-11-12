---
paths:
  - scripts/ci/test_fail_closed_ci.py
  - scripts/ci/apt-install-cached.sh
  - scripts/ci/tests/test-apt-install-cached.sh
  - scripts/ci/install-*.sh
  - scripts/dev/tidy-lane.sh
  - scripts/release/tests/test-docker-image-runtime-contract.sh
  - scripts/setup/*.sh
  - test/e2e/incus-kind-vm.sh
  - .github/workflows/*.yml
  - .github/actions/*/action.yml
  - Dockerfile*
  - Containerfile.vmafx
  - dev/Containerfile
  - docker/Dockerfile.*
  - docker/dev/ubuntu-*.Dockerfile
  - mcp-server/vmaf-mcp/Dockerfile
  - .pre-commit-config.yaml
invariant: ADR-3537 enforces fail-closed flags on every tracked apt refresh command (continuation lines joined).
area: gates
---
# APT index refresh fail-closed contract (ADR-3537)

`AptUpdateFailClosedContract` in `scripts/ci/test_fail_closed_ci.py` enforces
[ADR-3537](../../../docs/adr/3537-apt-update-fail-closed.md) across every
tracked apt refresh command (continuation lines joined) via
`apt_update_problems(path, text)` and `apt_update_contract_problems(root)`.

Scope = `ci_text_paths(root)` (`git ls-files -z`: `.yml`, `.yaml`, `.sh`,
`.bash`, `*.Dockerfile`, `*.Containerfile` suffixes; `Dockerfile*` and
`Containerfile*` basenames; `docker/OtherDockerfile` excluded; 1,864-file
bound; shared with ADR-3533). Reuses `fail-closed-ci-contract` hook in
`.pre-commit-config.yaml` and `DisposableRepoTestCase` fixture isolation.

Checker folds `\` continuations (200-line bound), tokenises with `shlex` across
operators (`;`, `&&`, `||`, `|`, `&`, `(`, `)`) and 8-item nested `-c` queue
(unbalanced quotes fail), and matches exact token `update`. Every refresh
requires lowercase `apt-get`, `--error-on=any`, and positive last
`Acquire::Retries`; non-regular tracked paths and workflow or action `sudo
apt-get ... update` calls fail closed. Comment mentions of `apt-get ... update`
fail closed too; reword comments to `apt index refresh`. Subcommand
`scripts/ci/apt-install-cached.sh refresh` writes `80ci-retries`, deletes runner
`*microsoft*` and `*azure*` source lists, and runs `sudo apt-get --error-on=any
-o Acquire::Retries=3 update`.

- `sudo` -> `bash`/`sh`/`dash`/`zsh`/`ksh` clause: nested `-c` inherits `sudo`.
- Hosted YAML parse failure or non-`|` multi-line `run` apt refresh -> error.

Diagnostics:

```text
<path>:<line>: apt-get update must fail on index errors: add --error-on=any -o Acquire::Retries=3 (ADR-3537)
<path>:<line>: hosted-runner apt refresh must run through scripts/ci/apt-install-cached.sh refresh or install (it drops the runner's Microsoft/Azure sources first; ADR-3537)
<path>:<line>: multi-line run scalar holding an apt refresh must be a literal block (run: |) so each command sits on its own line (ADR-3537)
<path>:<line>: hosted workflow or action YAML does not parse; the apt refresh contract cannot read its run blocks (ADR-3537)
<path>: tracked CI text path is not a regular file; the contract cannot read it
```
