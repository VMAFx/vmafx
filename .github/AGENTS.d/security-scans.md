---
paths:
  - .github/workflows/security-scans.yml
  - .semgrep.yml
  - requirements/locks/build.txt
invariant: .semgrep.yml owns Semgrep OSS; CodeQL builds in runner.temp; event in concurrency; every CodeQL language, every event.
---
# Security scans workflow invariants (Semgrep and CodeQL)

## Semgrep SARIF authority split (ADR-1314)

Only repository-owned `.semgrep.yml` result may be uploaded to GitHub Code
Scanning under required `Semgrep OSS` identity. moving
`p/cwe-top-25`, `p/c`, and `p/python` registry packs are advisory discovery
inputs: keep their scan `continue-on-error: true` and retain their SARIF as
ordinary workflow artifact. Never restore `semgrep-registry` Code Scanning
category without superseding ADR that provides reproducibly pinned policy.

Security Scans concurrency group includes `github.event_name` between
workflow name and ref. Scheduled scan and master push both use
`refs/heads/master`; without event discriminator, either can cancel
other's CodeQL coverage. On master the ref slot is the SHA and
`cancel-in-progress` is `github.ref != 'refs/heads/master'` (ADR-1673), so
only superseded PR runs collapse; keep
`scripts/ci/test_security_workflow_contract.py` in always-on Rules gate.

## Meson configure precedes CodeQL extraction (ADR-1222 / Alert 1279)

In [`security-scans.yml`](../workflows/security-scans.yml), `codeql-cpp` job
must execute `meson setup` before `github/codeql-action/init`, and must place
build directory outside repository checkout in `${{ runner.temp }}/build`.
configure step installs Meson and Ninja through repository's
hash-locked `requirements/locks/build.txt`; keep that root-relative lock path
when moving step out of `core/`.
Running configure outside extraction prevents Meson compiler probe test snippets
(such as `testfile.c`) from being ingested into CodeQL extraction database
(closing current hosted probe alert 1279, historical alert 1278, and pre-merge
alerts 1232–1235). Placing build root in `${{ runner.temp }}/build` ensures
generated build artifacts are not indexed as repository source. **On rebase or
workflow sync:** do not move `meson setup` after `codeql-action/init` or
configure within `$GITHUB_WORKSPACE`.

## Every CodeQL language on every event (ADR-3294)

`codeql-cpp`, `codeql-python`, `codeql-actions`: `init` + `analyze` steps carry
no `if:`; no impact planner in these jobs. Skipped category -> GitHub `CodeQL`
check neutral ("configuration not found") -> aggregator blocks (ADR-3293).
Draft skip via job-level `needs.tier.outputs.light` stays. Guard:
`scripts/ci/test_security_workflow_contract.py`
(`test_codeql_analyses_every_language_on_every_event`).
