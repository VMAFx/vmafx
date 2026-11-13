---
paths:
  - cmd/vmafx-tune/cmd/sidecar*.go
  - cmd/vmafx-tune/cmd/testdata/sidecar/**
invariant: Validation errors exit 2, state I/O exit 1; byte-identical Python fixtures + universal newlines.
---
<!-- markdownlint-disable MD013 MD029 -->
# `sidecar` subcommand exit statuses and Python parity fixtures

## Rebase-sensitive invariants

26. **`sidecar` group's exit statuses and fixtures = Python
    contract** (`cmd/vmafx-tune/cmd/sidecar.go`, `sidecar_parity_test.go`,
    `testdata/sidecar/`): every validation failure exits 2:
    `useUsageExitCode` for flag-layer errors; `requireFlags` instead of
    `MarkFlagRequired` for missing flags; `asUsageError` on codec / model /
    feature-file / capture-file paths. Cache-dir and `state.json` write
    failures stay exit 1, because Python `OSError` there uncaught. Do not
    wrap whole run function in `asUsageError`; split = contract.
    `TestSidecarPythonParity` diffs every stdout payload and every `state.json`
    snapshot byte-for-byte against fixtures dumped from Python CLI by
    `testdata/sidecar/regen.sh` (pinned host UUID, relative `--cache-dir`);
    regenerate them only with coordinated change on both sides. Stderr
    wording is not pinned, but `batch-record` skip-line numbers are:
    `splitLinesUniversal` reproduces CPython's `newline=None` iteration so
    they match, must not be swapped back for `bufio.Scanner` (caps
    line length, cannot split on lone `\r`).
