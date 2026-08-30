- Pre-commit hooks brought current where the bump is behaviour-safe: `isort`
  6.0.1 → **9.0.1** and `markdownlint-cli2` v0.22.1 → **v0.23.2**. The remaining
  pins were already at their latest upstream release (`pre-commit-hooks` v6.0.0,
  `black` 26.5.1, `shfmt` v3.13.1-1, `shellcheck-py` v0.11.0.1, `gitleaks`
  v8.30.1, `conventional-pre-commit` v4.4.0), verified against each repository's
  releases/tags rather than assumed.
- **`ruff` is deliberately NOT bumped here.** v0.15.17 → v0.16.5 rewrites **119
  files** automatically and leaves **106 violations** that need hand fixing
  across rules new to that release (RUF046 ×29, RUF059 ×17, PLW1510 ×9, PLR0917,
  BLE001, TRY004, S110 …). That is a refactor, not a dependency refresh, and it
  gets its own PR — same treatment as `clang-format` v22.1.5 → v23.1.0, which
  reformats the whole C tree.
