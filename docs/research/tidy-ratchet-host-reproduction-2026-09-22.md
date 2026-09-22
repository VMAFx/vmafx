<!-- markdownlint-disable MD013 -->
# Research: reproducing the ADR-1142 clang-tidy ratchet off the CI runner

**Date**: 2026-09-22
**Author**: `chore/hiss21-core-src-root` blocker pass

## Summary

`scripts/ci/tidy-ratchet.py --lane cpu` is a per-file ratchet, so a local
re-measurement has to reproduce the CI runner's toolchain, not merely *a*
clang-tidy. Measuring on an Arch-family host with the correct clang-tidy
(22.1.8) but the host's own glibc and GCC invents four `misc-static-assert`
findings in two files nobody touched, and a `--write` from that measurement
would silently raise their baselines. The fix is to measure in an Ubuntu 26.04
container with `gcc-15` and apt.llvm.org's `clang-tidy-22`, which is exactly
what the `Tidy Ratchet` job installs.

## What CI actually runs

`.github/workflows/lint-and-format.yml`, job `clang-tidy-ratchet`
(`name: Tidy Ratchet`, `runs-on: ubuntu-26.04`):

```bash
sudo apt-get install -y gcc-15 g++-15 ninja-build nasm jq
/tmp/llvm.sh 22 && sudo apt-get install -y clang-tidy-22
CC=gcc-15 CXX=g++-15 meson setup build core \
  -Denable_cuda=false -Denable_sycl=false -Db_lto=false
meson compile -C build
python3 scripts/ci/write-compile-commands.py --build-dir build
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
  --clang-tidy /usr/bin/clang-tidy-22 --jobs "$(nproc)"
```

Two details bite a local reproduction:

- **The build directory must be named `build` at the repository root.** The
  generated model translation units are recorded in the baseline by their
  build-relative path (`build/src/vmaf_v0.6.1.json.c`). Measuring out of
  `build-hiss/` renames all 18 of them, so every one reads as a new file and
  the 18 baselined entries read as removed.
- **The baseline stores `clang_tidy_version` and `cc_version`.** A mismatch is
  only a warning in `report()`, never a failure, so a wrong-toolchain
  measurement fails open into the numbers rather than being rejected.

## The host-glibc false positives

Measured on CachyOS (glibc 2.42-family, GCC 16.2.1) with clang-tidy 22.1.8, the
whole-tree run reports two regressions:

```text
error: core/src/dict.cpp: warnings 15 -> 16 (+1)
error: core/src/feature/feature_collector.cpp: warnings 13 -> 16 (+3)
```

All four extra diagnostics are `cert-dcl03-c,misc-static-assert` on ordinary
runtime assertions — `assert(d->size > 0)`, `assert(capacity > 0)`. Neither file
is touched by the branch, and both are byte-identical to master, so the source
cannot be the cause. It is not the language standard either: forcing
`--extra-arg=-std=c++23` and `-std=c++26` both reproduce it.

The cause is the `assert` macro. The host's `assert.h` selects the variadic C++
definition:

```c
#  if __ASSERT_VARIADIC
/* The first test of __VA_ARGS__ evaluates it without converting scoped
   enumeration values to bool, and the second test checks that it is a ... */
```

while Ubuntu 26.04's `assert.h` selects the older non-variadic form:

```c
#  define assert(expr)                          \
     (static_cast <bool> (expr)                 \
      ? void (0)                                \
```

`misc-static-assert` matches on the shape of the expanded assertion, so the
variadic expansion trips it on conditions the non-variadic one does not.
Re-measuring the identical tree in an Ubuntu 26.04 container with `gcc-15`
reports no regressions at all and no `cc_version` warning, which confirms the
attribution.

## Reproduction recipe

```bash
git clone -q --no-hardlinks --branch <branch> <repo> /path/to/clone
docker run -d --name tidy-repro -u 0 \
  -v /path/to/clone:/path/to/clone -w /path/to/clone \
  vmaf-dev-mcp:local sleep infinity
docker exec tidy-repro bash -lc '
  apt-get update -qq && apt-get install -y -qq wget gnupg lsb-release
  wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh
  /tmp/llvm.sh 22 && /usr/bin/clang-tidy-22 --version | grep -F "LLVM version 22."'
docker exec tidy-repro bash -lc '
  CC=gcc-15 CXX=g++-15 meson setup build core \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false
  meson compile -C build -j 4
  python3 scripts/ci/write-compile-commands.py --build-dir build
  python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
    --clang-tidy /usr/bin/clang-tidy-22 --jobs 4 --write'
```

The `vmaf-dev-mcp` image already carries Ubuntu 26.04, `gcc-15`, meson, ninja,
nasm and ONNX Runtime 1.30.0, so only `clang-tidy-22` has to be added. ONNX
Runtime matters: the baseline contains `core/src/dnn/*.c`, so a build without it
drops five translation units from the measurement. Use a throwaway container —
the long-lived `vmaf-dev-mcp` mounts one specific worktree read-only.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Full `--write` from the host measurement | Raises `dict.cpp` and `feature_collector.cpp` by +1/+3 on files the branch never touched. "Fix the code, never raise the baseline" — and the increase is not even real. |
| Scoped `--only … --write` (ADR-1243) for the improved TUs | Honest and guarded (it refuses to raise anything), but `merge_scoped_baseline` rewrites only the requested *sources*. The slack in `core/src/framesync.h` (2 -> 1) and `core/src/log.h` (1 -> 0) is attributed to headers, which no `--only` set can name, so the gate would still exit 3. |
| Pin the host to gcc-15 | CachyOS has no gcc-15 package; and it is the glibc `assert` macro, not the compiler version, that drives the difference. |
| Container with Ubuntu's own `clang-tidy-22` | Ubuntu 26.04 ships 22.1.2; the baseline records 22.1.8. apt.llvm.org gives 22.1.8, matching exactly. |

## Consequences for the next re-measurement

Record `clang_tidy_version` and `cc_version` agreement as the acceptance test
for a local ratchet write: if `report()` prints either "differs from baseline"
warning, the measurement is not comparable and must not be written.
