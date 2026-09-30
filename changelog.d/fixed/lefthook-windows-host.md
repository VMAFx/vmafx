- **Local hooks: the lefthook and pre-commit stack runs on Windows.** On Windows,
  lefthook passes each `run:` value to `sh -c` without escaping it, so the
  multi-line framework job ended at its first double quote and every commit
  failed in `framework-hooks`. The framework stages now run through
  `scripts/git-hooks/framework-hooks.sh`, which also finds a Windows virtualenv
  (`.venv/Scripts/pre-commit.exe`) and puts it on `PATH` for the checks. The
  pre-commit lock installs `reuse` with `charset-normalizer`, the only encoding
  detector reuse can use on Windows. Three checks that failed on Windows for
  path and line-ending reasons now pass there: the base-image local exceptions,
  the research-digest ID fixtures and `generate-adr-by-tag.sh --check`. The
  pre-push `govulncheck ./...` could not load `cmd/vmafx-node/bpf` outside
  Linux; that eBPF package now builds on Linux only, as ADR-0996 intended.
  `make install-hooks` now leaves lefthook's hooks in place, so the
  `commit-msg` and `pre-rebase` dispatchers install beside lefthook (ADR-1385).
  `.claude/settings.json` and `.codex/hooks.json` are stored in the form
  `lefthook uninstall` writes, so an uninstall no longer dirties them.
  [Local Git hooks](docs/development/pre-commit-hooks.md#windows-hosts)
  documents the Windows setup (`T-HOOKS-WINDOWS-LEFTHOOK-QUOTING-2026-09-30`).
