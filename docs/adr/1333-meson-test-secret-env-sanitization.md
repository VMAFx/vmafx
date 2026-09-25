<!-- markdownlint-disable MD013 MD060 -->

# ADR-1333: Meson test environment secret sanitization

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: lusoris
- **Tags**: `security`, `build`, `test`, `meson`, `ci`

## Context

Meson's test runner inherits the host process environment and records the resulting
environment in `build/meson-logs/testlog.json`. On workstations and CI runners, that
environment can contain GitHub credentials. The risk covers ordinary GitHub API tokens as
well as the Actions OIDC request bearer token and the Actions runtime access token.

Meson 1.12.1's installed `mesonbuild/mtest.py` also establishes two important precedence
rules. Selecting an alternate test setup replaces the default setup, and each test's `env`
is applied after the selected setup. A default setup therefore protects the tests declared
today, but an unguarded future alternate setup or per-test credential assignment could
bypass it.

## Decision

We protect the currently declared Meson tests at both the runtime and source-contract
boundaries:

1. **One default sanitizing setup**: `core/meson.build` declares the repository's only
   `add_test_setup`, named `default`, with `is_default: true`. Its environment unsets these
   twelve credential-bearing names:

   - `GITHUB_PERSONAL_ACCESS_TOKEN`
   - `GITHUB_TOKEN`
   - `GH_TOKEN`
   - `GH_ENTERPRISE_TOKEN`
   - `GITHUB_ENTERPRISE_TOKEN`
   - `GITHUB_PAT`
   - `GH_PAT`
   - `GITHUB_AUTH_TOKEN`
   - `GITHUB_API_TOKEN`
   - `HOMEBREW_GITHUB_API_TOKEN`
   - `ACTIONS_ID_TOKEN_REQUEST_TOKEN`
   - `ACTIONS_RUNTIME_TOKEN`

2. **Fail-closed declaration contract**:
   `core/test/test_meson_secret_env_sanitization.py` enumerates every production
   `core/**/meson.build`. It requires exactly one `add_test_setup`, requires every listed
   unset exactly once in the root file, and rejects any other explicit occurrence of a
   forbidden credential name. This prevents tracked alternate setups and explicit per-test
   restoration from silently bypassing the default.
3. **Minimal synthetic regression environment**: subprocess probes copy only the small
   platform-runtime allowlist `PATH`, `PATHEXT`, `SYSTEMROOT`, `SystemRoot`, `WINDIR`, and
   `COMSPEC` when present. Home, temporary-directory, locale, user, ordinary-control, and
   credential values are synthetic and private to each temporary directory. The tests never
   enumerate or copy the caller's remaining environment.
4. **Red-capable runtime proof**: hermetic Meson projects demonstrate the unmitigated child
   and JSON-log leak, prove the sanitizing default removes all twelve names, and explicitly
   demonstrate Meson's alternate-setup and per-test-environment precedence. The RED child
   succeeds only when it sees a forbidden key, and checks membership without reading values.

This is an explicit-name denylist, not a claim that arbitrary future credential names are
automatically discovered. A new credential-bearing name must be added to the production
unset list and the shared regression tuple together.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Default setup plus fail-closed source contract (chosen)** | Protects child processes and Meson's JSON log; no per-test wrapper; detects the two known tracked bypass forms. | An explicit denylist needs maintenance; a caller that edits build metadata outside the reviewed tree is out of scope. | Smallest repository-owned control that covers every currently declared test and detects configuration drift. |
| Default `add_test_setup` without a declaration contract | Native, fast, and platform-neutral. | An alternate setup or a per-test `env` can restore a key after sanitization. | The guarantee would be broader than Meson's precedence semantics support. |
| Custom test wrapper (`--wrapper`) | Can sanitize the child process. | Meson constructs and logs `result.env` outside the wrapper; debugger invocation becomes more complex. | Does not protect `testlog.json`. |
| CI-only stripping | Easy to add to one workflow. | Misses local, IDE, container, and future workflow entry points. | The defect is in the repository test boundary, not one workflow. |
| Clear the whole environment | Removes unknown credentials too. | Breaks executable lookup, platform runtime, temporary paths, locales, and toolchains. | Excessive and incompatible with the test suite. |

## Consequences

- The twelve listed credentials are absent from child environments and `testlog.json` for
  every currently declared test using the sole default setup.
- Tracked Meson changes that add another setup or explicitly spell a forbidden credential
  anywhere outside its sanctioned unset fail the regression contract.
- Ordinary host variables remain available to production tests; only the synthetic regression
  harness is hermetic and allowlisted.
- The denylist and its tests must be extended when another credential-bearing environment name
  becomes relevant.

## References

- req: "oh of course all bugs.md's in this local repo should of course be fully fixed"
- [GitHub OIDC reference: `ACTIONS_ID_TOKEN_REQUEST_TOKEN` is a bearer token](https://docs.github.com/en/actions/reference/security/oidc)
- [GitHub Actions runner source: runtime and OIDC token environment injection](https://github.com/actions/runner/blob/main/src/Runner.Worker/Handlers/NodeScriptActionHandler.cs)
- Installed Meson 1.12.1 source: `mesonbuild/mtest.py`, `merge_setup_options()` and
  `get_test_runner()`.
- Regression contract: `core/test/test_meson_secret_env_sanitization.py`.
