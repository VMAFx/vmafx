<!-- markdownlint-disable MD013 MD060 -->
# Research-1333: Meson test environment secret credential sanitization

## Scope

This digest details the investigation, root-cause diagnosis, falsification of alternative
hypotheses, and verification evidence for pre-RC1 security defect
`T-MESON-TEST-SECRET-ENV-LEAK-2026-09-25`: Meson test executions inheriting and persisting
plaintext secret environment variables (specifically `GITHUB_PERSONAL_ACCESS_TOKEN` and related
GitHub token variables) in `build/meson-logs/testlog.json`.

## Root cause diagnosis

Meson's test runner harness (`mesonbuild/mtest.py`) implements test execution via `SingleTestRunner`
and test logging via `JsonLogfileBuilder`.

1. **Environment inheritance**:
   In `mtest.py` (`SingleTestRunner.get_test_runner()`), when no test setup is defined, Meson initializes
   the test environment with `env = os.environ.copy()`. If any test-specific `env` is specified via
   `test(..., env: ...)`, it is merged on top of `env`.
2. **Log persistence**:
   When tests complete, `JsonLogfileBuilder.log()` records test metadata into `meson-logs/testlog.json`.
   The record explicitly includes `'env': result.env`, writing the entire key-value mapping of
   environment variables passed to the child process in plaintext JSON format.
3. **Credential exposure**:
   In CI runners, automated scripts, and developer workstations, credentials such as
   `GITHUB_PERSONAL_ACCESS_TOKEN`, `GITHUB_TOKEN`, and `GH_TOKEN` are standard environment variables.
   Every invocation of `meson test` or `ninja test` dumped these secrets to `testlog.json` and passed
   them directly into every unit test child process.

## Falsification of alternative hypotheses

1. **Hypothesis: A custom wrapper or project build target was injecting the token into test commands.**
   - *Falsification*: An audit of all `meson.build` files (`core/meson.build`, `core/test/meson.build`,
     `core/tools/test/meson.build`) confirmed no wrapper script or `add_test_setup` existed in the tree.
     The inheritance is native upstream Meson behavior in `mesonbuild/mtest.py`.
2. **Hypothesis: Meson has an upstream built-in secret scrubbing or exclusion mechanism.**
   - *Falsification*: Inspection of the installed Meson 1.12.1 source code (`/usr/lib/python3.14/site-packages/mesonbuild/mtest.py`)
     confirmed that Meson performs zero filtering or sanitization of environment keys or values prior to
     logging or spawning child processes.
3. **Hypothesis: Sanitizing via CI workflow YAML (`env: GITHUB_TOKEN: ""`) is sufficient.**
   - *Falsification*: Local developer runs, Docker/dev-MCP container sessions, and custom test runners
     would remain exposed. Furthermore, CI workflows often require `GITHUB_TOKEN` for checkout or runner
     probes; stripping it globally breaks runner orchestration.

## Options and trade-offs

| Approach | Coverage | Log Protection | Child Process Protection | Debugger / Tool Impact | Decision |
|---|---|---|---|---|---|
| Per-test wrapper script (`--wrapper`) | Partial (only wrapped tests) | No (`mtest.py` writes `result.env` regardless) | Yes | Breaks direct debugger attachment (`gdb`) | Rejected: does not solve `testlog.json` leakage. |
| Global shell wrapper in Makefile | Local `make test` only | No | Partial | Ineffective for IDE / raw `meson test` | Rejected: bypassable and non-durable. |
| Broad `os.environ.clear()` | Complete | Yes | Yes | Breaks required runtime (`PATH`, `HOME`, compiler libs) | Rejected: weakens and breaks test suites. |
| **Meson default `add_test_setup` with `unset()`** | **Repo-wide (`meson test`, `ninja test`, `make test`)** | **Yes (`result.env` stripped)** | **Yes (child never sees secrets)** | **Zero (native Meson feature, zero overhead)** | **Selected (ADR-1333).** |

## Implementation

In `core/meson.build`:

```meson
sanitized_test_env = environment()
sanitized_test_env.unset('GITHUB_PERSONAL_ACCESS_TOKEN')
sanitized_test_env.unset('GITHUB_TOKEN')
sanitized_test_env.unset('GH_TOKEN')
sanitized_test_env.unset('GH_ENTERPRISE_TOKEN')
sanitized_test_env.unset('GITHUB_ENTERPRISE_TOKEN')
sanitized_test_env.unset('GITHUB_PAT')
sanitized_test_env.unset('GH_PAT')
sanitized_test_env.unset('GITHUB_AUTH_TOKEN')
sanitized_test_env.unset('GITHUB_API_TOKEN')
sanitized_test_env.unset('HOMEBREW_GITHUB_API_TOKEN')
add_test_setup('default',
    env : sanitized_test_env,
    is_default : true,
)
```

Meson 1.4.0+ (the project floor) evaluates `EnvironmentVariables.unset()`, which removes the listed keys
from `full_env` during test preparation (`merge_setup_options` in `mtest.py`).

## Verification evidence

1. **Red-capable regression contract (`core/test/test_meson_secret_env_sanitization.py`)**:
   - `test_reproduce_red_unsanitized_leaks_token`: Runs vanilla Meson without test setup under injected
     synthetic credentials. Proves RED reproduction (`GITHUB_PERSONAL_ACCESS_TOKEN in logged_env == True`).
   - `test_green_sanitized_setup_excludes_secrets_and_preserves_required_env`: Runs Meson with default
     sanitizing setup. Proves GREEN resolution (all secret keys absent from child process and `testlog.json`;
     `PATH` and `VMAFX_TEST_REQUIRED_VAR` preserved; exit code 0).
   - `test_static_meson_build_sanitizes_credentials`: Asserts all 10 token variables are explicitly unset
     and non-secret variables are retained.
   - `test_live_process_environment_excludes_secrets`: Asserts that when executed under `meson test`, the test
     process environment contains zero forbidden credential variables.
2. **Value safety invariant**:
   - The regression reads only logs created in its private temporary directory with synthetic values.
     It never opens historical repository build logs, whose contents may include real credentials.
     Assertions operate on key presence and never print or hash token values.
