<!-- markdownlint-disable MD013 MD060 -->

# ADR-1333: Meson test environment secret sanitization

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: lusoris
- **Tags**: `security`, `build`, `test`, `meson`, `ci`

## Context

During pre-RC1 supply-chain and credential audits, a security defect was identified
in Meson test execution: the test runner (`mtest.py`) inherits `os.environ` from the host
process by default and copies the entire environment dictionary (`result.env`) into
`build/meson-logs/testlog.json` via `JsonLogfileBuilder.log()`.

On developer workstations, automated testing environments, and CI runners, sensitive GitHub
credential tokens such as `GITHUB_PERSONAL_ACCESS_TOKEN`, `GITHUB_TOKEN`, and `GH_TOKEN` are
routinely exported for API interaction, repository checkout, or self-hosted runner dispatch.
When `meson test`, `ninja test`, or `make test` runs:

1. The plaintext token values are inherited by all test child processes, exposing credentials
   to child binaries, core dump analyzers, or crash handlers.
2. Meson's `testlog.json` persists the complete environment mapping on disk in plaintext.
3. If test failure logs or build artifact bundles are archived, uploaded to CI artifact
   storage, or shared across developer machines, secret credentials risk exfiltration.

The current tracked repository tree contains no token patterns, and build logs are ignored in
`.gitignore`, but future test executions must be guaranteed not to inherit or persist
secret-bearing credential variables.

## Decision

We implement repo-wide secret credential sanitization for all Meson tests using Meson's
declarative test setup mechanism in `core/meson.build`:

1. **Default test setup with `environment().unset()`**:
   Meson 1.4.0+ (the project's declared minimum `meson_version: '>= 1.4.0'`) supports
   `EnvironmentVariables.unset()`. We configure a project-wide default test setup
   via `add_test_setup('default', env : sanitized_test_env, is_default : true)`.
2. **Explicit credential sanitization list**:
   We explicitly unset all standard GitHub secret and token variables:
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
3. **Preservation of necessary and non-secret environment**:
   We deliberately avoid broad deletion of arbitrary environment variables. Required runtime
   variables (`PATH`, `HOME`, `USER`, `TMPDIR`, compiler flags) and non-secret GitHub metadata
   variables (`GITHUB_ACTIONS`, `GITHUB_REPOSITORY`, `GITHUB_WORKSPACE`, `GITHUB_REF`,
   `GITHUB_SHA`) remain untouched so that normal test discovery, execution, and CI status checks
   function without degradation.
4. **Red-capable regression contract**:
   We register `core/test/test_meson_secret_env_sanitization.py` in the `fast` test suite.
   The regression test provides:
   - A RED proof demonstrating that vanilla Meson without the sanitizing test setup leaks
     secret credentials into child environments and `testlog.json`.
   - A GREEN proof demonstrating that the default test setup reliably purges all specified
     token variables from both test child processes and `testlog.json` while preserving
     ordinary required environment variables (`PATH`, custom test variables).
   - In-suite assertion that live test execution under Meson contains zero forbidden credentials.
   - Strict adherence to the security invariant: token values are never printed, read back,
     hashed, or exposed in assertions or failure messages.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Meson `add_test_setup` with `unset()` (chosen)** | Native to Meson 1.4.0+, applies universally to `meson test`, `ninja test`, and `make test`, zero process overhead, platform-neutral (Linux, macOS, Windows). | Requires test runs to use default setup or explicitly inherit it. | Cleanest and most durable repo-wide approach; operates directly at the Meson test harness layer where `testlog.json` is generated. |
| Custom test wrapper script (`--wrapper`) | Can sanitize environment before launching each test binary. | Introduces process-fork overhead for every unit test; does not sanitize Meson's own `testlog.json` log construction; complicates debugging under `gdb`/`lldb`. | Rejected because `mtest.py` records `result.env` before the wrapper runs, leaving `testlog.json` vulnerable. |
| CI-only environment stripping in YAML | Simple to implement in `.github/workflows/`. | Does not protect local developer runs, container sessions, or custom CI scripts; fragile to new workflow additions. | Rejected as insufficient; pre-RC1 security requires durable repository-level guarantees. |
| Broad `os.environ.clear()` | Guarantees complete isolation. | Breaks required runtime dependencies (`PATH`, dynamic linker paths, temp directories, locale settings). | Rejected per requirement to avoid broad deletion of arbitrary environment. |

## Consequences

- **Positive**:
  - Meson test child processes and `testlog.json` logs cannot inherit or persist any common
    GitHub personal access tokens or secret credentials.
  - Zero performance overhead: `unset()` removes dictionary keys in memory prior to process spawn.
  - Works consistently across local developer workstations, containers, and all CI matrices.
- **Negative**:
  - The explicit credential-name list must be extended when GitHub-compatible tooling adds a new
    token environment variable.
- **Neutral / follow-ups**:
  - Any future toolchain credential variables added to the project should be appended to the
    `sanitized_test_env` block in `core/meson.build`.

## References

- Prompt requirement: Fix Meson test logs inheriting and persisting `GITHUB_PERSONAL_ACCESS_TOKEN`.
- Pre-RC1 security bug tracking in `docs/state.md`.
- Regression contract: `core/test/test_meson_secret_env_sanitization.py`.
- Meson documentation: `add_test_setup` and `environment.unset` (Meson 1.4.0+).
