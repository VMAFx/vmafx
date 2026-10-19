## POSIX-only build parts off Windows, msvcism POSIX-header check (2026-10-08)

`fix/msvcism-posix-headers`, [ADR-2646](adr/2646-posix-only-build-options.md).

- `core/src/meson.build`: `enable_mcp=true` on Windows is a configure `error()`;
  `subdir('mcp')` and `compat/libvmaf/mcp.c` carry
  `host_machine.system() != 'windows'`. `core/test/meson.build`: the MCP tests
  carry the same gate, and `subdir('fuzz')` runs only off Windows (`fuzz=true` on
  Windows is an `error()`). `core/tools/meson.build`: the `vmaf_vpl` block carries
  the gate and prints a disabled message on Windows. An upstream sync that
  touches these blocks keeps the gates: the `msvcism` scan reads them to decide
  which sources the Windows build compiles.
- `scripts/dev/preflight.sh` resolves its scanners next to itself
  (`PREFLIGHT_DIR`) and fails the stage when `find-posix-only-headers.py` or
  `lint_exceptions.py filter` cannot run.
- Exceptions: `.config/lint-exceptions.d/msvcism-posix-headers.toml` (two files,
  expiry 2027-06-30).
- Reproducer: `bash scripts/ci/tests/test-preflight-msvcism.sh`;
  `scripts/dev/preflight.sh --full --stage msvcism`.
