<!-- markdownlint-disable MD013 MD060 -->

# ADR-3400: Pattern-based credential scrub and testlog scan for Meson test runs

- **Status**: Accepted
- **Date**: 2026-11-05
- **Deciders**: lusoris
- **Tags**: `security`, `build`, `test`, `meson`, `ci`

## Context

[ADR-1333](1333-meson-test-secret-env-sanitization.md) removes twelve listed GitHub and
Actions credential names before Meson starts. Meson 1.12.1 writes the parent environment of
the test runner to `meson-logs/testlog.txt` and each test's environment to `testlog.json`.
On 2026-11-05 a read-back over the workstation's build directories found GitHub token values
in testlog files of the main checkout and several linked worktrees. Two gaps let them in:

- **Unlisted names.** A credential exported under a name outside the list reaches both logs
  even through the wrapper. A synthetic probe (a `ghp_`-shaped value in `MY_ODD_PAT`) showed
  one match in `testlog.txt` and one in `testlog.json` through the ADR-1333 wrapper. The host
  exports at least one such name (a messaging token).
- **Direct runs.** A direct Meson run skips the wrapper. ADR-1333 records this as an
  unsupported bypass, but agent lanes and hand runs still use it, and the logs then sit in a
  gitignored build directory that hosted lanes and archives can read.

A test inside the suite cannot see the leak of its own run: Meson buffers `testlog.txt`, and a
probe test that read the file during the run found zero matches while the finished file held
the value.

## Decision

1. `scripts/ci/run_meson_test.py` removes every listed name and every name matching the
   credential pattern `TOKEN(?!IZER)|SECRET|PASSWORD|PASSWD|CREDENTIAL|API_?KEY|ACCESS_KEY|PRIVATE_KEY`,
   ending in `_PAT` (or equal to `PAT`), or holding an `AUTH` word between underscores or
   at an end (`DOCKER_AUTH_CONFIG`, `npm_config__auth`; not `AUTHOR`), ignoring case. It matches names only and never
   reads a value. The twelve-name list, the `core/meson.build` default setup and the ADR-1333
   inventory test stay unchanged, because Meson's `unset()` takes explicit names.
2. `scripts/ci/check_testlog_credentials.py` walks a checkout for `meson-logs/testlog*` files
   and fails when a line holds a credential-shaped value (GitHub `gh[pousr]_` and
   `github_pat_` tokens, `sk-` keys, AWS `AKIA` key ids). It prints paths and counts only.
   The pre-commit framework hook `testlog-credential-scan` runs it on framework-managed
   commits and pushes. The opt-in native commit hook (`VMAFX_NATIVE_HOOKS=1`) skips this
   scan, but its pre-push hook still runs it. A checkout that ran Meson directly cannot pass
   those framework-managed checks until the logs are deleted. A missing root, an unreadable
   directory or a log that is not a regular file fails with exit 2.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Pattern scrub plus pre-commit scan (chosen)** | Covers unknown names on the wrapper path and detects direct runs before the next commit. | The pattern can miss a name with no credential word; the scan detects after the fact. | Two independent barriers, each proven by a planted case. |
| Extend the explicit list only | Matches ADR-1333's shape. | Every new tool's name needs a change; the host already exports an unlisted one. | The list lags the environment by construction. |
| Scan inside the test suite | Runs with every test run. | Meson buffers `testlog.txt`; the probe saw zero matches during the run. | Cannot observe the leak it is meant to catch. |
| Clear the whole environment | Removes every credential. | Breaks toolchains, lookup paths and locales (ADR-1333). | Rejected in ADR-1333 for the same reasons. |

## Consequences

- Credentials under a matching name are absent from both logs when tests start through the
  wrapper. The protection covers the listed names, the name pattern and the scanned value
  shapes only; a credential under an unmatched name in an unrecognised shape still reaches
  the logs. A test that needs a matching variable must set it itself (none does today).
- A direct Meson run that writes a credential-shaped value fails the next commit in that
  checkout, with the log path and the remedy.
- A name with no credential word still passes the wrapper; the value-shaped scan is the
  second barrier for that case.

## References

- Controller order `vmafx-meson-testlog-env-scrub-20261105` (paraphrased: scrub credential
  variables from the Meson test environment and gate it with a planted case).
- [ADR-1333](1333-meson-test-secret-env-sanitization.md): the wrapper, default setup and
  entry-point contract this ADR extends.
- Operator guide: [Credential-safe Meson test runner](../development/meson-test-runner.md).
- Tests: `core/test/test_meson_secret_env_sanitization.py`,
  `scripts/ci/tests/test_check_testlog_credentials.py`.
