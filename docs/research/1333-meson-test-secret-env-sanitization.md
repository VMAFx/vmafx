<!-- markdownlint-disable MD013 MD060 -->
# Research-1333: Meson test environment secret credential sanitization

- **Status**: Active
- **Workstream**: [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md)
- **Last updated**: 2026-09-25

## Question

How can the repository keep credential-bearing environment variables out of both Meson test
children and `testlog.json`, while preserving the runtime environment tests need and detecting
future tracked configuration that bypasses the sanitizer?

## Sources

- Installed Meson 1.12.1 source at
  `/usr/lib/python3.14/site-packages/mesonbuild/mtest.py`, especially
  `merge_setup_options()` and `get_test_runner()`.
- [GitHub OIDC reference](https://docs.github.com/en/actions/reference/security/oidc), which
  classifies `ACTIONS_ID_TOKEN_REQUEST_TOKEN` as the bearer token used to request an OIDC token.
- [GitHub Actions runner `NodeScriptActionHandler`](https://github.com/actions/runner/blob/main/src/Runner.Worker/Handlers/NodeScriptActionHandler.cs), which injects both
  `ACTIONS_RUNTIME_TOKEN` and `ACTIONS_ID_TOKEN_REQUEST_TOKEN` from the runtime access token.

## Findings

### Meson inheritance and logging

Without a selected setup, `get_test_runner()` starts from `os.environ.copy()`. With a setup,
`merge_setup_options()` applies the setup environment to that same host copy. Meson then applies
the test-specific environment after the setup and passes the resulting dictionary to the child.
The JSON logger persists that result as the test entry's `env` mapping.

Consequently, a default `environment().unset()` removes a listed key from both the child and the
JSON log, but only under that selected setup.

### Precedence boundary

Two hermetic RED probes confirm the installed-source reading:

1. A project with a sanitizing default and an empty `unsafe` setup exposes synthetic credential
   keys when invoked with `meson test --setup=unsafe`.
2. A test-specific `env` applied after the sanitizing setup can restore a synthetic
   `GITHUB_TOKEN` key.

The production contract therefore enumerates all fourteen `core/**/meson.build` files, requires
exactly one `add_test_setup` in `core/meson.build`, and rejects every explicit forbidden-name
occurrence other than the twelve sanctioned unset calls. This makes the guarantee precise:
every currently declared test is protected, and either tracked bypass pattern makes the contract
fail.

### Credential inventory

The original ten-name list omitted two runner-issued access tokens:

- `ACTIONS_ID_TOKEN_REQUEST_TOKEN`, documented by GitHub as an OIDC-provider bearer token.
- `ACTIONS_RUNTIME_TOKEN`, populated from the Actions runtime service connection by the runner.

Both belong in the same denylist as the GitHub API and personal-access-token aliases. URL-only
companions such as `ACTIONS_ID_TOKEN_REQUEST_URL` and `ACTIONS_RUNTIME_URL` are not credentials and
remain available.

### Regression-harness isolation

Copying every host variable except known GitHub names is not a safe way to construct a security
test: it can capture unrelated credentials before Meson starts. The replacement copies only six
platform-runtime names when present. It supplies temp-local `HOME`, `TMPDIR`, `TEMP`, and `TMP`,
deterministic locale and user values, one ordinary control variable, and synthetic values for the
twelve credential names. A regression poisons an unrelated synthetic credential and proves it is
not copied.

The child probe only asks whether keys are present. It never indexes a credential key, reads a
credential value, prints the environment, or emits a value in a failure message. Command
diagnostics redact even the known synthetic probe string.

## Alternatives explored

| Approach | Child protection | JSON-log protection | Bypass handling | Result |
|---|---|---|---|---|
| Wrapper script | Yes | No | Per-test adoption required | Rejected. |
| CI environment stripping | One CI entry point | One CI entry point | New entry points drift | Rejected. |
| Default setup only | Yes | Yes | Alternate setup and per-test env remain | Insufficient alone. |
| Clear the whole environment | Yes | Yes | Broad but disruptive | Rejected. |
| **Default setup plus static declaration contract** | **Yes** | **Yes** | **Rejects tracked setup and explicit-name escape hatches** | **Selected.** |

## Verification evidence

`python3 -m unittest core.test.test_meson_secret_env_sanitization` exercises nine cases:

- the live tree's single-setup/twelve-unset contract;
- mutation rejection for an alternate setup;
- mutation rejection for explicit reintroduction of each of the twelve names;
- minimal allowlisted probe-environment construction;
- live in-suite key absence;
- an unmitigated child-and-log RED reproduction;
- the sanitizing default GREEN proof;
- alternate-setup precedence RED proof; and
- per-test-environment precedence RED proof.

Before the production list was repaired, that command failed specifically because
`ACTIONS_ID_TOKEN_REQUEST_TOKEN` and `ACTIONS_RUNTIME_TOKEN` had no unset declarations. After the
two declarations were added, all runnable cases passed; the live in-suite case correctly skips
when invoked directly rather than by Meson.

## Open questions

- GitHub-compatible runners may introduce additional credential-bearing names. Any such name
  requires source classification and a lockstep denylist/test update.

## Related

- [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md)
- `core/meson.build`
- `core/test/test_meson_secret_env_sanitization.py`
