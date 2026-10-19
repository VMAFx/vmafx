<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2828: Every CI build lane spells warnings as errors where praetor's build-warnings gate reads it

- **Status**: Accepted
- **Date**: 2026-10-09
- **Deciders**: lusoris
- **Tags**: ci, build, governance, standards

## Context

[ADR-2170](2170-warnings-are-errors-per-leg.md) turned warnings into errors leg by leg through one script, `scripts/ci/werror-args.sh`, called as `$(...)`, through a step output or from a matrix value. Praetor's build-warnings gate (HISS-10, in the pin `3a766f2d56ad`) reads the switch only as a literal on the build command, so [ADR-2784](2784-praetor-pin-3a766f2d.md) found 47 lanes in 16 workflows it read as ungated and declared each workflow in `.config/lint-exceptions.d/HISS-10.toml` until RC4 WP14 (#2438).

Measured on the last completed master runs (2026-10-08) and on this branch:

| Lanes | What they were | Measured |
| --- | --- | --- |
| 11 | gated by `werror-args.sh`, unread by the gate | the gated legs |
| 22 | Meson builds without the switch (golden, DNN, MCP smoke, coverage, coverage GPU, sanitizer matrix, Tidy Changed, Tidy SYCL, Cppcheck, Tidy Metal, CodeQL, nightly TSan and benchmark, fuzz, retrain CLI, Arc parity, the consumer comparison, the Rust-extractor build, Linux Intel LLVM, macOS Clang+Metal) | no compiler warning in the logs that compiled, except four `-Wreorder-init-list` in `integer_vif_metal.mm` and `float_ssim_metal.mm` (macOS) |
| 1 | Windows MSVC+SYCL (icx-cl) | two deprecated CRT calls in test headers (`fopen`, `_open`) |
| 5 | CMake builds of the third-party Level Zero loader | none; v1.34.0 builds with `-Wall -Werror` under gcc 16 |
| 1 | the static pkg-config link smoke (`${CC:-cc}`) | none with `-Wall -Wextra -Werror` under gcc 13, 15 and 16 |
| 2 | cgo steps without `-Werror` | `pkg/libvmaf` compiles with `-Wall -Werror` |
| 5 | cargo build, test and run without `-D warnings` | both workspaces check with `RUSTFLAGS=-D warnings --all-features --all-targets` |

## Decision

Spell the switch on every lane the gate reads (maintainer decision Q-316, option a):

- **Meson**: `-Dwerror=true` on `meson setup`, beside `$(scripts/ci/werror-args.sh true)` (or the `msvc` step output), which stays the one place that knows each linker's fatal-warnings switch. On the `cmd` legs the literal sits on the first line of the command, because the gate does not read a `^` continuation.
- **CMake**: `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` on the five Level Zero configures.
- **Direct compile**: the static link smoke names its compiler in the step's `env` (`ccache gcc-14`, checked against the row's) and compiles with `-Wall -Wextra -Werror`. Linking the LTO archive under those flags surfaced three `-Wmaybe-uninitialized` sites the library's own links never print: `float_vif.c` and `speed.c` ignored the result of `vif_get_scaling_method()` (validated at init, now checked), and `ssim.c` left the low-pass kernel's `bnd_const` unset (unused by `KBND_SYMMETRIC`, now 0). No computed value changes.
- **Go**: `CGO_CFLAGS: -O2 -g -Wall -Werror` on the `go-ci` job.
- **Cargo**: `RUSTFLAGS: -D warnings` on the `rust-ci` job.
- **Windows MSVC+SYCL**: `werror: msvc`; `_open()` in `owner_only_file.h` becomes `_sopen_s(..., _SH_DENYNO, ...)`, and every `fopen()` icx-cl reported in the tests (master job 113791454832: `vmafx_fixture_util.h`, `conversion_target_model.h`, `test_compat_conformance_scoring.c`, `test_vmafx_report.c`, `vmafx_score_contract.c`) goes through `core/test/test_fopen.h`, `_fsopen(..., _SH_DENYNO)` on Windows, for tests that cannot link `compat/path_utf8.c`.
- **Metal**: the two initialisers follow the struct's declaration order. On macOS `werror-args.sh` adds `-no_warn_duplicate_libraries` to the C++ link arguments: Meson's C++ standard-library probe of the Objective-C++ build links with `-lc++`, which clang++ adds again, and under `-Wl,-fatal_warnings` ld64's duplicate warning stopped `meson setup` ("Could not detect either libc++ or libstdc++") on the gated macOS Metal leg of master (run 37839768622) and on the newly gated macOS Clang+Metal leg of `build.yml`. `core/src/metal/meson.build` already gives the project's own links the switch (ADR-2170).
- **Meson configure**: the FFmpeg Windows MSVC job's configure printed eight pkg-config warnings, "Library target 'vmafx' has 'name_suffix' set. Compilers may not find it from its '-lvmafx' linker flag" (and `name_prefix`, and the same for `vmaf`, for the installed and the `-uninstalled.pc` files), because the static MSVC names of ADR-2752 are set through `name_prefix` / `name_suffix`, which Meson's pkg-config module always reports. Meson's own `namingscheme=platform` does not rename static libraries in Meson 1.12.1 (measured with a Windows cross build), so `core/src/meson.build` hands the module the flags `-L${vmafx_libdir} -lvmafx` / `-lvmaf` for that build, with `vmafx_libdir` set to `${libdir}` in the installed files and to the build directory in the uninstalled ones; the `Libs` a consumer reads are unchanged. `check_msvc_library_names.py --meson-log` fails a configure log that holds the warning, on the FFmpeg MSVC job and the static MSVC legs.
- **Analysers**: `scripts/ci/write-compile-commands.py` drops `-Werror` from the database clang-tidy and cppcheck read; the build of those lanes keeps it.

`HISS-10.toml` is removed; `praetorctl audit` passes with 50 lanes gated (49, plus the FFmpeg Windows MSVC job that #2642 added) and no exception. `T-CI-PRAETOR-HISS10-LANES-2026-10-08` and `T-CI-WARNINGS-MSVC-LEGS-2026-10-07` close.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the 16 entries until praetor reads a declared wrapper (cordanaLLM/praetor#924) | no workflow change | 36 lanes stay ungated meanwhile; entries renew every 90 days | Q-316 asked for both; this is option (a), #924 is option (c) |
| Move the switch out of `werror-args.sh` (literal only) | no duplicate `-Dwerror=true` | the linker switch per OS needs a home; ADR-2170's test contract changes | the duplicate is harmless (Meson takes the last value) |
| Replace the five Level Zero builds by one script | HISS-19 | the gate does not read a script, so the lane would pass unread | the gate must see the switch |
| Keep `-Werror` in the analysis database | one flag set | clang-tidy would turn clang's diagnostics on gcc's flags into errors | the database is read, not compiled |
| Gate release and tester image builds too | uniform | a newer compiler would stop a release (ADR-2170) | only the tester bundle's Level Zero build is a lane the gate reads, and it is gated |

## Consequences

- **Positive**: a warning in any lane the gate reads fails the pull request that adds it, and the audit proves the switch is present; the exception list is empty.
- **Negative**: a toolchain or third-party (Level Zero) update that adds a diagnostic fails the lane until it is fixed; `upstream-consumers.yml` builds the base commit with `-Werror`, which a base older than the gate could fail.
- **Neutral / follow-ups**: legs proven only on CI are listed in `docs/development/ci.md`; the dev container's third-party build stays outside the gate (`T-CI-WARNINGS-NON-MSVC-LEGS-2026-10-07`).

## References

- Q-316: both options; (a) one follow-up pull request that writes the switch literally on every lane next to `werror-args.sh`, gates the 36 ungated lanes, fixes the warnings that surface without weakening anything, and removes the HISS-10 entries; (c) a praetor issue asking for a declared wrapper (orchestrator ledger, 2026-10-09).
- Q-061: warnings are errors in every language ([ADR-2342](2342-rc-map-amendment-2026-10.md), RC4 WP14, #2438).
- [ADR-2170](2170-warnings-are-errors-per-leg.md), [ADR-2784](2784-praetor-pin-3a766f2d.md); cordanaLLM/praetor#924.
