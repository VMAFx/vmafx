---
paths:
  - .github/workflows/*.yml
invariant: clang-tidy beyond 18 requires llvm.sh archive setup; cc.find_library requires -dev package with unversioned symlink.
---
# Hosted runner package installation traps (LLVM and Level Zero)

## `clang-tidy-<N>` always needs the apt.llvm.org repo

Ubuntu 24.04 (`ubuntu-24.04`; `ubuntu-latest` until rollout from 2026-10-19)
ships **clang-tidy-18** at most. `ubuntu-26.04` image 20260927.149.1 ships
clang-tidy 20, 21, 22 (actions/runner-images `Ubuntu2604-Readme.md`, read
2026-10-11); image set moves, so rule stays. Every job installing newer
`clang-tidy-<N>` must add LLVM archive first:

```yaml
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 22
sudo apt-get install -y clang-tidy-22
```

Listing `clang-tidy-22` in plain `apt-get install` line aborts whole
step with `E: Unable to locate package clang-tidy-22`.

**This failure hides itself.** These jobs are gated on
`if: steps.detect.outputs.files != ''`, so on any PR changing no file in
scope every step is skipped and job reports **success**. Broken apt
line is only reached when job has real work, so run history looks
mostly green while gate has never once executed. `Clang-Tidy SYCL (Changed
Files, Advisory)` sat in exactly that state from LLVM 22 bump
(PR #1161, PR #1200) until it was fixed: 7 green no-op runs, 2 red runs,
zero SYCL files ever linted.

When bumping clang-tidy major, grep workflow for **every**
`clang-tidy-<old>` occurrence, confirm each one is preceded by
`llvm.sh` step. Verify job's green run did real work before
trusting it.

## `cc.find_library('foo')` needs the `-dev` package, not the runtime one

meson's `cc.find_library('foo')` emits literal `-lfoo`. `ld` resolves `-lfoo`
against `libfoo.so` or `libfoo.a` **only** — unversioned linker symlink
that lives in `-dev` package. Versioned runtime SONAME `libfoo.so.1`
that runtime package ships is invisible to `-l`, so installing runtime
package alone leaves probe failing with
`/usr/bin/ld: cannot find -lfoo` and meson's
`ERROR: C shared or static library 'foo' not found`.

Concretely, for Level Zero loader on `ubuntu-24.04`:

```yaml
sudo apt-get install -y libze-dev   # libze_loader.so + level_zero/ze_api.h
# NOT libze1 — that ships only libze_loader.so.1
```

Two traps around this one:

- **oneAPI does not supply it.** `intel-oneapi-compiler-dpcpp-cpp` plus
  `source /opt/intel/oneapi/setvars.sh --force` still leaves `-lze_loader`
  unresolvable. Loader is separate, vendor-neutral dispatch library.
- **Package name is release-specific.** It is `libze-dev` on 24.04
  `noble` (source package `level-zero`, in `universe`, already enabled on
  hosted image). `level-zero-dev` does **not** exist on noble; never copy
  that name from newer release or from comment written for one.

Loader links with no GPU present, never calls `zeInit`, so no
accelerator, no `intel-level-zero-gpu`, and no device-plugin resource is
needed for configure/compile lane. Never reach for Intel's graphics APT
repository to satisfy `-lze_loader`; Intel's oneAPI APT repository contains no
`level-zero` packages at all.

`.github/workflows/libvmaf-build-matrix.yml` solves same requirement
differently — it builds `oneapi-src/level-zero` from source at pinned tag,
because it links shipping artifact, wants known loader version.
static-analysis lane needing only probe to resolve should prefer
distro package.
