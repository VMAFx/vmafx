<!-- markdownlint-disable MD013 -->

# Credential-safe Meson test runner

Run the native test suite through the repository wrapper from the repository root:

```bash
python3 scripts/ci/run_meson_test.py -- -C core/build
python3 scripts/ci/run_meson_test.py -- -C core/build --suite=fast
```

`make test`, `make test-fast`, the checked-in CI workflows, the preflight script, and the
checked-in IDE task use the same wrapper. Arguments after `--` pass directly to `meson test`.
To select a particular Meson installation, put its executable before that separator:

```bash
python3 scripts/ci/run_meson_test.py \
  --meson-executable /path/to/meson -- -C core/build --print-errorlogs
```

## Why the wrapper is required

Meson 1.12.1 writes its parent process environment to `meson-logs/testlog.txt` before it
applies any `add_test_setup()` environment changes. A Meson test setup can therefore remove
credentials from test children and `testlog.json`, but it cannot remove them from the text
log. The wrapper deletes the twelve GitHub and GitHub Actions credential names governed by
[ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md) before Meson starts. It checks
and deletes keys without reading or retaining their values.

The default setup in `core/meson.build` remains a second defense. It protects test children
and the JSON log if Meson receives a clean parent environment, and the regression contract
rejects alternate setups or per-test assignments that restore a governed name.

## Boundary and direct-command bypass

Direct `meson test`, `ninja ... test`, and `meson compile ... test` invocations bypass the
parent-process sanitizer and can write credential-bearing variables to `testlog.txt`. They
are unsupported as repository test entry points. Use the wrapper or the Make targets above.
This repository cannot control an arbitrary command typed outside its wrappers.

The control is an explicit-name denylist, not automatic secret discovery. When a runner or
tool introduces another credential-bearing environment name, update all three surfaces in
one change:

1. `scripts/ci/run_meson_test.py`;
2. the default setup in `core/meson.build`; and
3. `core/test/test_meson_secret_env_sanitization.py`.

The regression suite uses disposable build directories and synthetic markers. It verifies
both `testlog.txt` and `testlog.json` without reading, copying, or printing caller credential
values. Probe subprocesses default to 120 seconds to avoid false-positive timeout flakes under
CPU/IO contention. `VMAFX_MESON_TEST_TIMEOUT_SECONDS` accepts only finite values from 60 through
300 seconds; malformed, non-finite, too-low, and too-high overrides fail closed so configuration
cannot remove the hard hang boundary.
