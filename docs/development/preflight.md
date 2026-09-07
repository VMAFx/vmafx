<!-- markdownlint-disable MD013 MD041 -->

# `make preflight` — the local gate that matches CI

```bash
make preflight                       # everything, on the files you changed
scripts/dev/preflight.sh --full      # everything, on the whole tree
scripts/dev/preflight.sh --stage m32 # just one stage
scripts/dev/preflight.sh --list      # which CI context each stage mirrors
```

Run it before you push. `make lint` and `meson test` build with **one**
compiler; CI builds with several, and the gap is where portability bugs live.

## Why it exists

On 2026-09-07 a single branch shipped three portability breaks in one
afternoon. All three were green under local gcc:

| What was written | Locally | In CI |
| --- | --- | --- |
| `static_assert(UINT_MAX <= SIZE_MAX/2/sizeof(ptr))` | fine on 64-bit | `Ubuntu i686 gcc` cannot compile it — the claim is false on 32-bit |
| `__declspec(align((x)))` | gcc never compiles the MSVC branch | `Windows MSVC+CUDA` → C2059 on every use; `align()` needs a literal |
| `__attribute__(noinline)` | gcc accepts the single paren | `Ubuntu clang`, `clang+DNN` and four Sanitizer lanes fail to compile |
| `nullptr` in a `.c` file | gcc and clang accept it under `-std=c23` | `Windows MSVC+CUDA` → C2065 at every site; ADR-1138 keeps C TUs on `NULL` |
| `static const double` used in a `static` aggregate initialiser | no diagnostic at all, even with `-pedantic-errors -Weverything` | `Windows MSVC+CUDA` → C2099, then cascading C2440s as the members shift |
| `M_PI` in a new test file | glibc exposes it because meson passes `-D_GNU_SOURCE` | `Windows MinGW64` → `'M_PI' undeclared`; MinGW ignores `_GNU_SOURCE` and `-std=c23` sets `__STRICT_ANSI__` |

The last three are from the same evening: PR #1340's new test file carried the
first two (21 errors on `Windows MSVC+CUDA`), and PR #1342's carried the third.
All three are new *test* files, which is the pattern — a test is the easiest
place to write portable-looking C that only one lane rejects.

Each cost a full CI round-trip. Because only one PR is in flight at a time,
that is queue time for every other PR too — the branch held the merge window
for about three hours.

## Stages

| Stage | Mirrors | Catches |
| --- | --- | --- |
| `gcc` | Ubuntu gcc(+DNN) | the baseline build and fast tests |
| `clang` | Ubuntu clang(+DNN) | clang-only syntax; gcc is far more permissive about attributes and extensions |
| `m32` | Ubuntu i686 gcc | assumptions about the width of `size_t` / `ptrdiff_t` / pointers |
| `msvcism` | Windows MSVC+CUDA / +SYCL | constructs MSVC rejects, checked statically so no MSVC is needed |
| `sanitizers` | Sanitizers (address) / (undefined) | UB the plain build hides |
| `tidy` | Tidy Changed | clang-tidy on the touched files, using the workflow's own exclusion list |
| `cppcheck` | Cppcheck | cppcheck's findings |

A stage whose toolchain is missing is **skipped with a notice**, not failed, so
the script is still useful on a partially provisioned machine. For the 32-bit
stage you want `gcc-multilib`.

## Two behaviours worth knowing

**It looks at uncommitted work.** Not just `origin/master...HEAD` — the edit
you are about to commit is exactly what you want checked. (The first version of
this script did not, and cheerfully passed against a deliberately planted
break.)

**`sanitizers` needs `-Db_lundef=false`.** Clang links the sanitizer runtime
into executables, not shared libraries, so `libvmaf.so` is left with undefined
`__asan_report_*` / `__ubsan_handle_*` symbols and `-Wl,--no-undefined` refuses
the link. The repo's own `fuzz.yml` pairs the same two options. Without it the
stage fails on every branch, including ones that change no code at all — and a
stage that cries wolf is a stage people learn to ignore.

**`m32` needs a configured build.** It compiles `-fsyntax-only` with
`-I build/src` so the generated `config.h` resolves. Without that the compile
aborts at the first `#include` and the stage silently becomes a no-op — run the
`gcc` stage first, or just use `make preflight`, which orders them correctly.

## What it does not cover

`Windows MinGW64`, the Windows MSVC lanes proper, and `Ubuntu HIP` have no
local equivalent here. `msvcism` is a set of pattern matches plus one small
scanner (`scripts/dev/find-nonconst-static-init.py`) over known rejection
classes, not a compiler — `Windows MSVC+*` remains the authority. For HIP and
the other GPU backends use the dev container
([dev-mcp.md](dev-mcp.md), ADR-0451).

## Keeping it honest

If you add a required CI context, add a stage or record why it cannot be
mirrored. `--list` is the single place that mapping is written down, and
[ADR-1234](../adr/1234-local-preflight-gate.md) records the reasoning.
