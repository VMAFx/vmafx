# ADR-3375: Semgrep runs outside the repository's Python locks

- **Status**: Accepted
- **Date**: 2026-10-25
- **Deciders**: Lusoris
- **Tags**: ci, security, supply-chain, python

## Context

`requirements/locks/tooling-tests.txt` carried `semgrep==1.182.1` so that the
Tooling Tests job could run `scripts/ci/tests/test_semgrep_vendored_scope.py`.
Semgrep pins its own dependencies exactly: 1.182.1, the newest release, requires
`mcp==1.29.0` and `opentelemetry-api~=1.37.0`. Both carry published advisories
(GHSA-84m7-p3x7-pcfv for `mcp`, and the alerts on `opentelemetry-api`), and
Dependabot reports each one against the lock. No reachable `semgrep` release
allows the fixed versions, so the alerts could only be declared with an expiring
`dependency-advisory` exception and an `osv-scanner.toml` ignore, renewed every
few days.

Semgrep is a scanner, not a library the repository imports. Two other sources of
it already exist and are pinned: the `semgrep-local` and
`test-semgrep-vendored-scope` pre-commit hooks install it into their own
isolated environment, and the required `Semgrep` CI job runs a digest-pinned
image from `.github/workflows/security-scans.yml`, which Renovate maintains.

## Decision

Semgrep leaves every lock under `requirements/locks/`. The vendored-scope test
takes `semgrep` from `PATH` when it is there (the hook environment), and
otherwise runs the image the `Semgrep` job pins, read from `security-scans.yml`,
through Docker with `--network=none` and the scanned copy mounted read-only.
With neither available the test fails; it never skips.
`requirements/locks/tooling-tests.in` pins `jsonschema` explicitly, because the
winget manifest tests imported it only through `semgrep`. The `mcp` exception
entry and the GHSA-84m7 ignore are removed: the packages they covered are no
longer in any lock.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `semgrep` in the lock with dated exceptions per advisory | No test change | Every semgrep pin bump renews exceptions; each new advisory of a semgrep dependency opens alerts again | Treats the symptom; the order asked for the root fix |
| Own `requirements/semgrep` lock outside `requirements/locks/` | Plain `pip install` in CI | Dependabot scans every manifest in the repository, so the alerts move rather than close | Does not close the alerts |
| Hook environment or pinned image (chosen) | No Python lock carries semgrep; one pinned image for CI and the test; the hook already pins its range | The test needs Docker where the hook environment is absent | Chosen |

## Consequences

- **Positive**: `mcp`, `opentelemetry-*` and about fifty other packages leave
  the tooling-tests lock; Dependabot alerts on them close instead of being
  declared; one Semgrep version (the pinned image) backs the CI job and the
  test.
- **Negative**: a Tooling Tests runner without `semgrep` on `PATH` pulls the
  Semgrep image once.
- **Neutral / follow-ups**: a Renovate bump of the image digest moves the test
  with it; the `dependency-advisory` rule stays available for a future advisory
  with no fix.

## References

- Supersedes the `mcp` entry recorded in
  `changelog.d/security/mcp-ghsa-84m7-exception.md`.
- Controller order `vmafx-dependabot-open-alerts-20261025` (paraphrased): clear
  the open Dependabot alerts by taking semgrep out of the scanned Python locks,
  without dismissing any.
