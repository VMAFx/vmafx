- **Native bash pre-commit hook (opt-in)** at
  `scripts/githooks/pre-commit.sh` plus installer
  `scripts/githooks/install.sh`. Drops local-commit overhead from
  ~3 s/hook (pre-commit-framework venv-wrap) to ~0.4 s total on a
  typical 3-formatter commit. Opt in via
  `VMAFX_NATIVE_HOOKS=1 make install-hooks`; default install path
  is unchanged (framework hook), CI is unchanged. The native hook
  runs `ruff check --fix`, `clang-format -i`, and
  `shfmt -w -i 2 -ci` on staged files only, mirroring the
  framework hook's file-scope rules. Missing binaries degrade
  gracefully (one stderr notice, never blocks the commit).
  Makefile target `hooks-install` renamed to `install-hooks` with
  the old name retained as a legacy alias. See
  [`docs/development/pre-commit-hooks.md`](docs/development/pre-commit-hooks.md)
  and [ADR-0924](docs/adr/0924-native-pre-commit-hooks.md).
