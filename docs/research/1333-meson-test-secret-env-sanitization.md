<!-- markdownlint-disable MD013 MD060 -->
# Research-1333: Meson test environment secret credential sanitization

- **Status**: Active
- **Workstream**: [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md)
- **Last updated**: 2026-09-25

## Question

How can the repository keep credential-bearing environment variables out of Meson's parent
text log, per-test JSON log, and test children while preserving the runtime environment tests
need and detecting future tracked entry points or declarations that bypass the sanitizer?

## Sources

- Installed Meson 1.12.1 source at
  `/usr/lib/python3.14/site-packages/mesonbuild/mtest.py`, especially
  `TextLogfileBuilder.start()`, `merge_setup_options()`, and `get_test_runner()`.
- [GitHub OIDC reference](https://docs.github.com/en/actions/reference/security/oidc), which
  classifies `ACTIONS_ID_TOKEN_REQUEST_TOKEN` as the bearer token used to request an OIDC token.
- [GitHub Actions runner `NodeScriptActionHandler`](https://github.com/actions/runner/blob/main/src/Runner.Worker/Handlers/NodeScriptActionHandler.cs), which injects both
  `ACTIONS_RUNTIME_TOKEN` and `ACTIONS_ID_TOKEN_REQUEST_TOKEN` from the runtime access token.

## Findings

### Meson inheritance and logging

`TextLogfileBuilder.start()` serializes `os.environ.items()` immediately after the text logger
opens. That happens before Meson creates a test runner or applies a selected setup. Without a
selected setup, `get_test_runner()` later starts from `os.environ.copy()`. With a setup,
`merge_setup_options()` applies the setup environment to that same host copy. Meson then applies
the test-specific environment after the setup and passes the resulting dictionary to the child.
The JSON logger persists that effective result as the test entry's `env` mapping.

Consequently, a default `environment().unset()` removes a listed key from both the child and the
JSON log, but only under that selected setup. It is too late to affect `testlog.txt`. A hermetic
probe with only synthetic credentials confirmed this exact split: child and JSON clean, text log
still carrying every synthetic key and marker.

### Parent-process boundary

`scripts/ci/run_meson_test.py` deletes governed keys before replacing itself with Meson. It uses
membership checks and mapping deletion; it does not index, retain, print, or copy credential
values. The repository's Make targets, CI workflows, preflight path, setup guidance, bisection
scaffold, and Zed task now enter Meson through this boundary.

The static contract inventories every supported wrapper call and scans executable entry-point
scopes for direct `meson test`, `ninja ... test`, and `meson compile ... test` forms. It
recursively includes all three GNU Make names (`GNUmakefile`, `makefile`, and `Makefile`) and
POSIX/Windows script types. GitHub workflow/action `run` values are extracted and normalized from
inline, literal, folded, and plain multiline YAML scalars with plain or quoted keys (`run:`,
`'run':`, `"run":`) before shell segmentation, rather than scanning physical YAML lines that do
not represent the executed command. Python entry points are parsed with bracket- and quote-aware
implicit line continuation to govern multi-line list/tuple command constructions. Executable
normalization covers quoted and path-qualified Meson/Ninja names plus Windows `.exe` spellings.
Logical-line handling joins shell, PowerShell, and batch continuations and checks each
separator-delimited command after removing only the wrapper-path token. Mutation coverage replaces
every inventoried call, one at a time, and proves each bypass is rejected. Adversarial fixtures
also reject a wrapper followed by `; meson test`, multiline workflow bypasses, Python implicit
continuations, path-qualified executables, and unsafe alternate Makefile names or Windows scripts.
A separate assertion compiles the checked-in pre-commit path filter and proves that every scanned
entry-point source, every Meson declaration, and representative future files trigger the contract.

This boundary cannot intercept an arbitrary command typed outside repository wrappers. Direct
raw Meson or Ninja test-target invocation remains an explicit unsupported bypass and is stated in
the operator guide and ADR.

### Precedence boundary

Two hermetic RED probes confirm the installed-source reading:

1. A project with a sanitizing default and an empty `unsafe` setup exposes synthetic credential
   keys when invoked with `meson test --setup=unsafe`.
2. A test-specific `env` applied after the sanitizing setup can restore a synthetic
   `GITHUB_TOKEN` key.

The production declaration contract therefore enumerates all fourteen `core/**/meson.build` files, requires
exactly one `add_test_setup` in `core/meson.build`, and rejects every explicit forbidden-name
occurrence other than the twelve sanctioned unset calls. This makes the guarantee precise:
every currently declared test reached through a supported entry point is protected at both
layers, and either tracked declaration bypass pattern makes the contract fail.

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
not copied. Subprocess probes default to a 120-second timeout
(`PROBE_SUBPROCESS_TIMEOUT_SECONDS`) that avoids false-positive flakes under heavy machine
contention. The optional override is validated as a finite value in the inclusive 60--300 second
range; invalid or out-of-range input fails closed rather than weakening the hang boundary.

The child probe only asks whether keys are present. It never indexes a credential key, reads a
credential value, prints the environment, or emits a value in a failure message. A mapping
sentinel also makes the wrapper test fail on any attempted value read. Command diagnostics redact
even the known synthetic probe string. Log assertions read only fixture-owned logs created in a
temporary directory from this synthetic environment; no caller log or credential is opened.

## Alternatives explored

| Approach | Child protection | JSON-log protection | Bypass handling | Result |
|---|---|---|---|---|
| Parent-process repository wrapper | Yes | Yes, both formats | Entry-point inventory rejects drift | Selected with the default setup. |
| Meson `--wrapper` child wrapper | Yes | No parent text-log protection | Per-test child only | Rejected. |
| CI environment stripping | CI children | CI logs | Other entry points drift | Rejected. |
| Default setup only | Yes | JSON only | Text log plus alternate setup and per-test env remain | Insufficient alone. |
| Clear the whole environment | Yes | Yes | Broad but disruptive | Rejected. |
| **Parent wrapper plus default setup and both contracts** | **Yes** | **Yes, text and JSON** | **Rejects tracked caller and declaration escape hatches** | **Selected.** |

## Verification evidence

`python3 -m unittest core.test.test_meson_secret_env_sanitization` exercises twenty-eight cases:

- exact wrapper/Meson credential-inventory agreement;
- deletion without credential-value reads;
- live supported-entry-point inventory and direct-call scan;
- pre-commit trigger coverage for every scanned source and Meson declaration scope;
- per-call mutation rejection across every supported entry point;
- rejection of newly added raw entry points, including quoted, path-qualified, and Windows
  executable spellings;
- rejection of a raw sibling after the wrapper and shell, PowerShell, and batch continuations;
- folded, literal, and plain multiline workflow scalar rejection across plain and quoted keys,
  plus multiline runner discovery, governance, and mutation rejection;
- Python implicit list/tuple continuation rejection for raw Meson/Ninja invocations, runner
  acceptance, and fail-closed syntax error behavior;
- bounded, load-tolerant probe subprocess timeout verification;
- recursive alternate-Makefile and Windows-script inventory aligned with the pre-commit filter;
- the live tree's single-setup/twelve-unset contract;
- mutation rejection for an alternate setup;
- mutation rejection for explicit reintroduction of each of the twelve names;
- minimal allowlisted probe-environment construction;
- live in-suite key absence;
- an unmitigated child-and-both-logs RED reproduction;
- the default-setup RED proof showing that only the text log still leaks the synthetic parent;
- the repository-runner GREEN proof across child, text log, and JSON log;
- alternate-setup precedence RED proof; and
- per-test-environment precedence RED proof.

Before the production list was repaired, that command failed specifically because
`ACTIONS_ID_TOKEN_REQUEST_TOKEN` and `ACTIONS_RUNTIME_TOKEN` had no unset declarations. After the
two declarations were added, that narrow suite passed while still missing the parent text-log
boundary. After adding the parent runner and both-log assertion, all runnable cases pass; the
live in-suite case correctly skips when invoked directly rather than by Meson.

## Open questions

- GitHub-compatible runners may introduce additional credential-bearing names. Any such name
  requires source classification and a lockstep denylist/test update.
- Raw external Meson/Ninja commands cannot be intercepted by repository source; keep them
  documented as unsupported rather than broadening the guarantee.

## Related

- [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md)
- `core/meson.build`
- `core/test/test_meson_secret_env_sanitization.py`
- `scripts/ci/run_meson_test.py`
- [Credential-safe Meson test runner](../development/meson-test-runner.md)
