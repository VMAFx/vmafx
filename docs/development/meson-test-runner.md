<!-- markdownlint-disable MD013 -->

# Credential-safe Meson test runner

Run the native test suite through the repository wrapper from the repository
root:

```bash
python3 scripts/ci/run_meson_test.py -- -C core/build
python3 scripts/ci/run_meson_test.py -- -C core/build --suite=fast
```

`make test`, `make test-fast`, the checked-in CI workflows, the preflight
script, and the
Zed task "Dev container: CPU fast gate" (`.zed/tasks.json`) use the same
wrapper. Arguments after `--` pass directly to `meson test`.
To select a particular Meson installation, put its executable before that
separator:

```bash
python3 scripts/ci/run_meson_test.py \
  --meson-executable /path/to/meson -- -C core/build --print-errorlogs
```

## Why the wrapper is required

Meson 1.12.1 writes its parent process environment to `meson-logs/testlog.txt`
before it
applies any `add_test_setup()` environment changes. A Meson test setup can
therefore remove
credentials from test children and `testlog.json`, but it cannot remove them
from the text
log. The wrapper deletes the twelve GitHub and GitHub Actions credential names
governed by
[ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md) before Meson starts. It checks
and deletes keys without reading or retaining their values.

The default setup in `core/meson.build` remains a second defense. It protects
test children
and the JSON log if Meson receives a clean parent environment, and the
regression contract
rejects alternate setups or per-test assignments that restore a governed name.

## Boundary and direct-command bypass

Direct `meson test`, `ninja ... test`, and `meson compile ... test` invocations
bypass the
parent-process sanitizer and can write credential-bearing variables to
`testlog.txt`. They
are unsupported as repository test entry points. Use the wrapper or the Make
targets above
(including for the single-test commands shown in [build flags](build-flags.md)
and
[oneAPI install](oneapi-install.md), which should be run through the wrapper).
For agent sessions, the Claude Code and Codex command hooks
(`.claude/hooks/block-unsafe-bash.sh`, `.codex/hooks/block-unsafe-bash.sh`)
refuse these direct calls: direct `meson test`, `ninja` with `test` as a
separate target word, and `meson compile` with `test` as a separate target word.
The match stops at `;`, `|`, `&` and a newline, so a build line followed by
another command is allowed. The repository ships no such hook for Gemini CLI
(its `.gemini/settings.json` runs the engine's `praetorctl hook gemini pre-tool`),
Antigravity or opencode, and a shell outside an agent session is unguarded too,
so the pre-commit `testlog-credential-scan` remains.

The wrapper removes the twelve listed names plus every name that matches a
credential pattern ([ADR-3400](../adr/3400-meson-testlog-credential-patterns.md)):
any name containing `TOKEN` (but not `TOKENIZER`, as in
`TOKENIZERS_PARALLELISM`), `SECRET`, `PASSWORD`, `PASSWD`, `CREDENTIAL`,
`API_KEY`, `APIKEY`, `ACCESS_KEY` or `PRIVATE_KEY`, any name that is `PAT`
or ends in `_PAT`, and any name with an `AUTH` word such as
`DOCKER_AUTH_CONFIG` (never `AUTHOR`), in any letter case. Only names are matched; values are never
read. A credential exported under another name, such as `MY_ODD_PAT` or
`LITELLM_API_KEY`, therefore no longer reaches either log through the wrapper.
The default setup in `core/meson.build` still unsets only the twelve listed
names, because Meson can unset only names it is given. When a runner or tool
introduces another credential-bearing name that the pattern misses, update all
three surfaces in one change:

1. `scripts/ci/run_meson_test.py`;
2. the default setup in `core/meson.build`; and
3. `core/test/test_meson_secret_env_sanitization.py`.

## Finding logs written by a direct run

A direct run still leaves the values in the gitignored build directory. The
pre-commit hook `testlog-credential-scan` walks the checkout for
`meson-logs/testlog*.txt` and `testlog*.json` and fails when a line holds a
credential-shaped value (GitHub `ghp_`/`gho_`/`ghu_`/`ghs_`/`ghr_` tokens,
`github_pat_` tokens, `sk-` keys, AWS `AKIA` key ids). It prints the log path
and a line count, never a value. Run it by hand on any checkout:

```bash
python3 scripts/ci/check_testlog_credentials.py            # current folder
python3 scripts/ci/check_testlog_credentials.py ../other   # another root
```

On a finding, delete the named `meson-logs` directory and rerun the tests
through the wrapper. Exit codes: 0 clean, 1 a finding, 2 a read error, a
missing root, a log that is not a regular file, or a bound exceeded.

The scan recognises the value shapes listed above only. A credential in
another shape, such as the base64 registry login in `DOCKER_AUTH_CONFIG`,
is invisible to it; for those the wrapper's name pattern is the only barrier.
The opt-in native pre-commit hook (`VMAFX_NATIVE_HOOKS=1`) does not run the
scan at commit time; the framework pre-push hook still runs it.

## Maintaining the regression test

The regression suite (`core/test/test_meson_secret_env_sanitization.py`) uses
disposable build directories and synthetic markers. It verifies both
`testlog.txt` and `testlog.json` without reading, copying, or printing caller
credential values. Its probes run your `meson` in a small synthetic
environment. Three details matter when a probe fails:

- **`HOME`**: a temporary directory, so no caller state leaks in.
- **`PYTHONUSERBASE`**: Python normally finds `pip install --user` packages
  under `HOME`, so the probes set `PYTHONUSERBASE` to the user base the test
  interpreter resolved (an explicit host `PYTHONUSERBASE` is carried
  unchanged). This extends ADR-1333's platform-runtime allowlist by one path
  so that a Meson installed with `--user`, as `scripts/setup/ubuntu.sh` does,
  works the same as a system or virtualenv install. If the probes fail with
  `No module named 'mesonbuild'`, check that `meson` on `PATH` and the Python
  that runs the test share that user base.
- **Timeout**: probe subprocesses default to 120 seconds to avoid
  false-positive timeout flakes under CPU/IO contention.
  `VMAFX_MESON_TEST_TIMEOUT_SECONDS` accepts only finite values from 60
  through 300 seconds. Malformed, non-finite, too-low, and too-high overrides
  fail closed so configuration cannot remove the hard hang boundary.
