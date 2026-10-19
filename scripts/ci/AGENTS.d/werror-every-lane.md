---
paths:
  - scripts/ci/werror-args.sh
  - scripts/ci/tests/test_werror_args.py
  - scripts/ci/write-compile-commands.py
invariant: Every CI build lane spells its warnings-as-errors switch on the command; `werror-args.sh` adds only the linker half.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Warnings as errors on every lane (ADR-2170, ADR-2828)

Praetor build-warnings gate (HISS-10, `praetorctl audit`) reads switch only as
literal on build command, never `$(werror-args.sh ...)` output, step output or
matrix value.

- Meson lane: `-Dwerror=true` on `meson setup` beside
  `$(scripts/ci/werror-args.sh true)` (linker fatal-warnings switch per OS).
  `cmd` legs: literal on first line of command; gate reads no `^`
  continuation. Matrix legs keep `werror:` key for `werror-args.sh`.
- CMake lane (Level Zero): `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON`. cgo:
  `CGO_CFLAGS` with `-Werror` on `go-ci` job. Cargo: `RUSTFLAGS: -D warnings`
  on `rust-ci` job. Static link smoke: compiler named in step `env`, checked
  against row.
- `write-compile-commands.py` drops `-Werror` from analysis database
  (clang-tidy, cppcheck); build keeps flag. Never drop other flags there.
- `HISS-10.toml` empty. New lane spells switch; `praetorctl audit` fails lane
  lacking switch. Fix warning at source (no `-Wno-*`, no pragma); exception only
  for lane not yet gateable: one workflow, reason, expiry.
- Release and tester image builds stay off werror (ADR-2170); tester bundle
  Level Zero build = gate lane, gated.
