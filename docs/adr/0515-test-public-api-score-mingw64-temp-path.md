<!-- markdownlint-disable MD060 -->
# ADR-0515: Portable temp-path setup for `test_public_api_score` on MinGW64

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: ci, build, windows, mingw, test, fork-local, bugfix

## Context

The `libvmaf Build Matrix — Linux/macOS/Windows/ARM × CPU/SYCL/CUDA`
workflow's `Build — Windows MinGW64 (CPU) (MINGW64, mingw-w64-x86_64)` job
has been red on master ever since `test_public_api_score.c` was added by
the 2026-05-16 coverage-audit closure. The compile succeeds; the failure
is in `meson test`:

```text
test_vmaf_write_output: fail, mkstemp failed
3 tests run, 1 failed
12/61 fast - libvmaf:test_public_api_score        FAIL            0.24s   exit status 1
```

Root cause: the test hardcodes a `/tmp/vmaf_test_output_XXXXXX` template
and calls `mkstemp(3)` on it. MSYS2/MinGW64 inside the GitHub Actions
`windows-latest` runner does not surface a usable `/tmp` from the
`MINGW64` shell — the path resolves outside the MSYS root mapping and
`mkstemp` fails with `ENOENT`. The Linux and macOS legs were green
because `/tmp` exists natively there; the contributor of the test had no
Windows leg in their local matrix.

This is a Windows-portability gap in a fork-added test (not in any
Netflix-golden assertion and not in library code). It blocks every
master push from going green on the MinGW64 leg.

## Decision

Extract the temp-file path setup into a helper
`make_temp_output_path(out, out_len)` that mirrors the precedent set in
`core/test/dnn/test_model_loader.c::test_sidecar_parses` (added for
the same MinGW64 reason):

- **On Windows** (`#ifdef _WIN32`): query `GetTempPathA()` and
  synthesise `"<TMP>\vmaf_test_output_<pid>"`; pre-create the file with
  `fopen(path, "w")` so the later `fopen("r")` + `ftell` sanity-read
  works.
- **On POSIX**: keep the `/tmp/vmaf_test_output_XXXXXX` + `mkstemp`
  path so the test retains its `O_CREAT|O_EXCL` atomic-create semantics
  there (no behaviour change on the green legs).

Replace `unlink(tmp)` with the cross-platform `remove(tmp)` (Windows
`unistd.h` is absent; `<stdio.h>` `remove` works the same on both).
Pull the `Win32` portability dance into the helper rather than inlining
it in the test body so `test_vmaf_write_output` stays under the
`readability-function-size` threshold without a NOLINT.

Scope is deliberately minimal: only `test_public_api_score.c` is
touched; the `dnn/test_model_loader.c` precedent stays as-is (it
already gates correctly).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Gate the whole test out of MinGW64 via meson `if host_machine.system() != 'windows'` | One-line fix | Loses Windows coverage of the three public C-API entry points the test was added to cover (the whole point of the audit closure). Hides a real portability gap. | Rejected — drops the test's coverage on the platform that surfaced the bug. |
| Use POSIX `mkstemp` on MinGW64 (MSYS provides one) but rewrite the template to a runtime-resolved temp dir via `getenv("TMPDIR")`/`getenv("TEMP")` | Slightly simpler, no Windows API call | MinGW64 `mkstemp` is from `<unistd.h>` and present, but on the `windows-latest` runner the MSYS environment leaks `TEMP=D:\...\Temp` (backslashes), which collides with the `XXXXXX` suffix expectations of `mkstemp`. Fragile. | Rejected — `GetTempPathA` matches the precedent in `dnn/test_model_loader.c` and is the canonical Win32 API for this. |
| Mark the test as `expected_to_fail` on MinGW64 | Zero code change in the test | Doesn't fix the bug; the test would still run and the failure assertion would just be inverted. Adds permanent skip-list debt. | Rejected — leaves CI red on the failure type, just renames the failure. |

## Consequences

- **Positive**: MinGW64 build matrix goes green; master push CI is no
  longer perpetually failing on the Windows leg. The three public-API
  entry points (`vmaf_score_at_index`, `vmaf_model_collection_load`,
  `vmaf_write_output`) retain test coverage on Windows.
- **Negative**: Adds a small `#ifdef _WIN32` branch + helper to the
  test file. The Win32 path uses a deterministic `<pid>`-suffixed
  filename rather than `mkstemp`'s random suffix, so two concurrent
  invocations of the same test binary in the same temp dir could
  collide; meson schedules each test as a single subprocess so this
  is not a real race in practice.
- **Neutral / follow-ups**: No follow-ups. The pattern matches the
  existing `test_sidecar_parses` precedent; future tests that need a
  temp file on Windows should call `make_temp_output_path` or copy
  the pattern into their own helper.

## References

- Workflow: `.github/workflows/libvmaf-build-matrix.yml` (MinGW64 leg at
  lines 731–820).
- Failing job example: `gh run view --repo VMAFx/vmafx 26021988312`
  (job `76485707408`) — `test_public_api_score → test_vmaf_write_output:
  fail, mkstemp failed`.
- Precedent: `core/test/dnn/test_model_loader.c::test_sidecar_parses`
  (uses `GetTempPathA` on `#ifdef _WIN32`).
- Source: `req` — operator brief identifying the MinGW64 build as
  pre-existing-red on master and requesting the conservative-scope
  unblock.
