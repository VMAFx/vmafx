<!-- markdownlint-disable MD013 MD041 -->

# `make preflight` — the local gate that matches CI

```bash
make preflight                       # everything, on the files you changed
scripts/dev/preflight.sh --full      # everything, on the whole tree
scripts/dev/preflight.sh --stage clang # just one stage
scripts/dev/preflight.sh --list      # which CI context each stage mirrors
```

Run it before you push. `make lint` and `make test-fast` build with **one**
compiler; CI builds with several, and the gap is where portability bugs live.

## Why it exists

Each portability break below was green under local gcc and cost a full CI
round-trip. Because only one PR is in flight at a time, that is queue time
for every other PR too: on 2026-09-07 a single branch shipped the first three
breaks in one afternoon and held the merge window for about three hours.

| # | What was written | Locally | In CI |
| --- | --- | --- | --- |
| 1 | `static_assert(UINT_MAX <= SIZE_MAX/2/sizeof(ptr))` | fine on 64-bit | the i686 lane of the time could not compile it (that lane is retired: the fork is 64-bit only, ADR-1258) |
| 2 | `__declspec(align((x)))` | gcc never compiles the MSVC branch | `Windows MSVC+CUDA` → C2059 on every use; `align()` needs a literal |
| 3 | `__attribute__(noinline)` | gcc accepts the single paren | `Ubuntu clang`, `clang+DNN` and four Sanitizer lanes fail to compile |
| 4 | `nullptr` in a `.c` file | gcc and clang accept it under `-std=c23` | `Windows MSVC+CUDA` → C2065 at every site; ADR-1138 keeps C TUs on `NULL` |
| 5 | `static const double` used in a `static` aggregate initialiser | no diagnostic at all, even with `-pedantic-errors -Weverything` | `Windows MSVC+CUDA` → C2099, then cascading C2440s as the members shift |
| 6 | `M_PI` in a new test file | glibc exposes it because meson passes `-D_GNU_SOURCE` | the MinGW lane (now `Windows UCRT64`) → `'M_PI' undeclared`; MinGW ignores `_GNU_SOURCE` and `-std=c23` sets `__STRICT_ANSI__` |
| 7 | `const ulong half = ...` in a `.metal` shader | the C / CUDA / HIP twins it was ported from have no such reserved name | `macOS Clang+Metal` → `cannot combine with previous 'type-name' declaration specifier`; `half` is MSL's 16-bit float type |

Rows 5 to 7 are from the same evening. PR #1340's new test file carried
rows 5 and 6 (21 errors on `Windows MSVC+CUDA`), and PR #1342's carried
row 7. Row 5 arrived on the same branch as row 4.

All are new files added by a PR, which is the pattern: fresh code is where
portable-looking constructs that exactly one lane rejects get written.

## Stages

| Stage | Mirrors | Catches |
| --- | --- | --- |
| `gcc` | Ubuntu gcc(+DNN) | the baseline build and fast tests |
| `clang` | Ubuntu clang(+DNN) | clang-only syntax; gcc is far more permissive about attributes and extensions |
| `msvcism` | Windows MSVC+CUDA / +SYCL | constructs MSVC rejects, checked statically so no MSVC is needed |
| `sanitizers` | Sanitizers (address) / (undefined) | UB the plain build hides (the required `Sanitizers (thread)` context is CI-only) |
| `tidy` | Tidy Changed | clang-tidy on the touched files, using the workflow's own exclusion list |
| `cppcheck` | Cppcheck | cppcheck's findings |

A stage whose toolchain is missing is **skipped with a notice**, not failed, so
the script is still useful on a partially provisioned machine.

There is no 32-bit stage. The fork is 64-bit only, and the `m32` stage went
with the i686 lane it mirrored (ADR-1258).

## Behaviours worth knowing

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

## What it does not cover

`Windows UCRT64`, the Windows MSVC lanes proper, and `Ubuntu HIP` have no
local equivalent here. `msvcism` is a set of pattern matches plus three small
scanners over known rejection classes, not a compiler — `Windows MSVC+*`
remains the authority. `scripts/dev/find-nonconst-static-init.py` finds a
`static` aggregate initialised from a `const` object (MSVC C2099).
`scripts/dev/find-posix-only-headers.py` finds a POSIX-only header
(`<unistd.h>`, `<dlfcn.h>`, `<sys/socket.h>`, ...) outside a platform
conditional in a source the Windows build compiles (MSVC C1083); it reads the
`meson.build` files to skip sources kept off Windows
(`host_machine.system() != 'windows'`), and a scan that cannot run fails the
stage. A file that is POSIX-only by design and built by no `meson.build` is a
named exception in `.config/lint-exceptions.d/msvcism-posix-headers.toml`
([ADR-2646](../adr/2646-posix-only-build-options.md)).
`scripts/dev/find-windows-minmax.py` finds a `std::min(`, `std::max(` or
`std::numeric_limits<T>::max(` call that a C++ translation unit of the Windows
build reaches after `<windows.h>` without `NOMINMAX` (MSVC C2589: the header
defines `min` and `max` as function-like macros). It reads each translation
unit in the order a Windows preprocessor would, through the tree's own headers
and the `core/src/compat/win32/` shims (`<pthread.h>` includes `<windows.h>`
there), and reports a unit only when the scan of it read a changed file; a
scan that cannot run fails the stage. `(std::min)(a, b)` and `std::min<T>(a, b)`
are no macro invocations and pass. It does not follow system or SDK headers,
does not scan CUDA device code or C sources, does not expand macro bodies and
does not see a `NOMINMAX` given on the compiler command line. For HIP and
the other GPU backends use the dev container
([dev-mcp.md](dev-mcp.md), ADR-0451).

## Keeping it honest

If you add a required CI context, add a stage or record why it cannot be
mirrored. `--list` is the single place that mapping is written down, and
[ADR-1234](../adr/1234-local-preflight-gate.md) records the reasoning.
