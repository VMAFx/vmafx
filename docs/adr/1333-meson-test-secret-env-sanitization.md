<!-- markdownlint-disable MD013 MD060 -->

# ADR-1333: Meson test environment secret sanitization

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: lusoris
- **Tags**: `security`, `build`, `test`, `meson`, `ci`

## Context

Meson's test runner inherits the host process environment. Meson 1.12.1's
`TextLogfileBuilder.start()` serializes that raw parent environment to
`build/meson-logs/testlog.txt` before it selects or applies a test setup. The JSON logger
later records each test's effective environment in `testlog.json`. On workstations and CI
runners, either boundary can contain GitHub credentials. The risk covers ordinary GitHub API
tokens as well as the Actions OIDC request bearer token and the Actions runtime access token.

Meson 1.12.1's installed `mesonbuild/mtest.py` also establishes two important precedence
rules. Selecting an alternate test setup replaces the default setup, and each test's `env`
is applied after the selected setup. A default setup therefore protects the tests declared
today, but an unguarded future alternate setup or per-test credential assignment could
bypass it.

## Decision

We protect supported repository test entry points at the parent-process, per-test, and
source-contract boundaries:

1. **Sanitize before Meson starts**: every supported repository entry point runs
   `scripts/ci/run_meson_test.py`. The wrapper deletes the following twelve
   credential-bearing keys from its process without reading or retaining their values, then
   replaces itself with `meson test`:

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

2. **Retain one default sanitizing setup**: `core/meson.build` declares the repository's only
   `add_test_setup`, named `default`, with `is_default: true`, and unsets the same twelve
   names. This second layer protects child processes and the JSON log against a future caller
   that starts from a clean parent but modifies test declarations.
3. **Fail-closed entry-point and declaration contracts**:
   `core/test/test_meson_secret_env_sanitization.py` enumerates every production
   `core/**/meson.build`. It requires exactly one `add_test_setup`, requires every listed
   unset exactly once in the root file, and rejects any other explicit occurrence of a
   forbidden credential name. This prevents tracked alternate setups and explicit per-test
   restoration from silently bypassing the default. It also inventories every wrapper call
   in the Makefile, CI workflows, preflight and setup scripts, bisection scaffold, and Zed
   task. It recursively inventories GNU Make's `GNUmakefile`, `makefile`, and `Makefile` names,
   plus POSIX and Windows script entry points. Workflow and action `run` values are normalized
   from inline, literal, folded, and plain multiline YAML scalar forms with plain or quoted keys
   (`run:`, `'run':`, `"run":`) before the scanner checks logical commands. Python entry points
   are parsed with bracket- and quote-aware implicit line continuation to govern list- and
   tuple-based command constructions. Raw `meson test`, `ninja ... test`, and `meson compile ... test`
   bypasses remain forbidden when an executable is quoted, path-qualified, or spelled with a Windows
   `.exe` suffix, split by explicit or implicit continuation, or placed beside the wrapper after
   a shell separator. Mutation tests replace each inventoried wrapper call in turn and require the
   contract to fail. Synthetic test probes enforce a bounded, load-tolerant subprocess deadline
   (`PROBE_SUBPROCESS_TIMEOUT_SECONDS`, 120 s default); overrides are finite and restricted to
   60--300 seconds. This prevents false-positive flakes on busy runners while continuing to catch
   hangs. The
   pre-commit hook's path filter covers every scanned source scope and every Meson declaration,
   including representative future files, so adding a bypass in a newly tracked supported entry
   point runs the contract.
4. **Minimal synthetic regression environment**: subprocess probes copy only the small
   platform-runtime allowlist `PATH`, `PATHEXT`, `SYSTEMROOT`, `SystemRoot`, `WINDIR`, and
   `COMSPEC` when present. Home, temporary-directory, locale, user, ordinary-control, and
   credential values are synthetic and private to each temporary directory. The tests never
   enumerate or copy the caller's remaining environment. Subprocess probes default to 120 seconds
   to avoid load-sensitive timeout failures during compiler discovery and test execution under
   heavy machine contention. The optional timeout override accepts only finite values from 60
   through 300 seconds and fails closed otherwise, preserving a hard upper bound.
5. **Red-capable runtime proof**: hermetic Meson projects demonstrate that raw Meson records
   the synthetic parent keys in `testlog.txt` even when the default setup protects the child
   and JSON log. The GREEN probe starts through the repository wrapper and requires all twelve
   key names and the synthetic marker to be absent from both log formats and the child.
   Separate RED probes retain Meson's alternate-setup and per-test-environment precedence
   evidence. Children check key membership without reading values.

This is an explicit-name denylist, not a claim that arbitrary future credential names are
automatically discovered. A new credential-bearing name must be added to the wrapper, the
production unset list, and the regression tuple together.

Direct raw `meson test`, `ninja ... test`, or `meson compile ... test` commands outside the
checked-in entry points remain an explicit bypass. The repository cannot sanitize an arbitrary
command typed by a caller, so those forms are unsupported for repository test runs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Parent wrapper plus default setup and fail-closed contracts (chosen)** | Removes keys before either log is opened; retains child/JSON defense; checks every supported caller and declaration. | An explicit denylist and entry-point inventory need maintenance; a raw external command remains possible. | Only repository-owned design that protects Meson's pre-setup text logger and its later per-test boundary. |
| Default `add_test_setup` without a declaration contract | Native, fast, and platform-neutral. | An alternate setup or a per-test `env` can restore a key after sanitization. | The guarantee would be broader than Meson's precedence semantics support. |
| Meson `--wrapper` child wrapper | Can sanitize the child process. | Meson records its parent and constructs per-test logs outside the child wrapper. | Cannot protect the parent text log. |
| CI-only parent stripping | Protects checked-in CI. | Misses Make, preflight, IDE, bisection, and local invocations. | The defect is in the repository test boundary, not one workflow. |
| Clear the whole environment | Removes unknown credentials too. | Breaks executable lookup, platform runtime, temporary paths, locales, and toolchains. | Excessive and incompatible with the test suite. |

## Consequences

- The twelve listed credentials are absent from the Meson parent, child environments,
  `testlog.txt`, and `testlog.json` when tests start through a supported repository entry
  point.
- Tracked entry-point changes that invoke the test target without the wrapper, and tracked
  Meson changes that add another setup or explicitly spell a forbidden credential outside its
  sanctioned unset, fail the regression contract.
- Ordinary host variables remain available to production tests; only the synthetic regression
  harness is hermetic and allowlisted.
- A caller that runs raw Meson or Ninja directly bypasses the parent sanitizer. This bounded
  limitation is documented in the developer guide rather than hidden behind a broader claim.
- The denylist and its tests must be extended when another credential-bearing environment name
  becomes relevant.

## References

- req: "oh of course all bugs.md's in this local repo should of course be fully fixed"
- [GitHub OIDC reference: `ACTIONS_ID_TOKEN_REQUEST_TOKEN` is a bearer token](https://docs.github.com/en/actions/reference/security/oidc)
- [GitHub Actions runner source: runtime and OIDC token environment injection](https://github.com/actions/runner/blob/main/src/Runner.Worker/Handlers/NodeScriptActionHandler.cs)
- Installed Meson 1.12.1 source: `mesonbuild/mtest.py`, `TextLogfileBuilder.start()`,
  `merge_setup_options()`, and `get_test_runner()`.
- Operator guide: [Credential-safe Meson test runner](../development/meson-test-runner.md).
- Regression contract: `core/test/test_meson_secret_env_sanitization.py`.
