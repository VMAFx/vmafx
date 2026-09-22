# Configured clang-tidy warning exit audit

## Question

Can `make lint-c` report success while clang-tidy prints findings for configured
native sources?

## Reproduction

The configured-lint fixture was extended with a clang-tidy stand-in that emits
an ordinary diagnostic and returns zero, matching clang-tidy's default warning
semantics. Before the fix, the driver printed `clang-tidy [0]` for every source
and returned success. The new regression failed with `0 != 1`, proving that the
gate observed only process status and not the warning condition it claimed to
enforce.

Installed clang-tidy 22.1.8 documents `--warnings-as-errors=<string>` as the
option that upgrades matching warnings to errors. `*` covers every diagnostic
selected by the repository configuration, including compiler diagnostics.

## Alternatives considered

| Option | Result |
| --- | --- |
| Parse human-readable logs | Rejected: formatting and summary text vary by clang-tidy version. |
| Depend only on `.clang-tidy` `WarningsAsErrors` | Rejected: command-line callers can drift and the configured driver owns this gate's fail-closed contract. |
| Add `--warnings-as-errors=*` to every invocation | Chosen: native tool semantics, version-documented, and independently regression-tested. |
| Retain a baseline or touched-file exception | Rejected: it preserves the false-green whole-tree blind spot. |

## Result

The driver now promotes every selected warning to a non-zero analyzer result,
records the `*` policy in `result.json`, prints the per-source log, continues to
Cppcheck, and fails the combined gate. No diagnostic suppression, baseline, or
source-origin exemption was added.

## Verification

```bash
python3 -m unittest \
  scripts.ci.tests.test_lint_configured.ConfiguredLintTests.test_clang_tidy_warning_is_promoted_to_gate_failure
python3 -m unittest scripts.ci.tests.test_lint_configured
```
