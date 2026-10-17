## Praetor pin 9bf502ec6ce5 and the engine launcher in the hooks (2026-10-12)

`build/engine-launcher-adoption`, [ADR-3272](adr/3272-praetor-pin-9bf502ec-engine-launcher.md). A
rebase or sync keeps `PRAETOR_REF` at `9bf502ec6ce5...` in `.github/workflows/standards-gate.yml`,
and keeps the seven governance jobs of `lefthook.yml`, `scripts/git-hooks/hiss-audit.sh` and
`scripts/githooks/state-sync.sh` on `sh .config/lefthook/engine.sh <arguments>`: a side of a
conflict that calls `praetorctl` or `standardsctl` directly brings back the engine from `PATH`.
`.config/lefthook/engine.sh` and `.config/praetor/engine.mk` are praetor's text. `praetorctl adopt`
refreshes each only while it is unedited, so never edit or format them: take them from one side of a
conflict, or regenerate them with `adopt --lock-source-root=<praetor clone at the pin>` in a
throwaway clone. `praetorctl audit` does not compare these two files or
`.config/agent/hooks/block_evasion.py`; it does compare `tools/figures/mkdocs_hook.py` byte for byte.
Keep the launcher's two dated exceptions (`.config/lint-exceptions.d/shfmt.toml`, `spdx.toml`) and
the three places that skip it for shfmt in step (the `exclude` of the `shfmt-src` hook,
`SHFMT_FILES` in the `Makefile`, the shell branch of `scripts/githooks/pre-commit.sh`);
`scripts/ci/tests/test_shfmt_scope.py` fails when they differ. The `PreToolUse` entries `adopt`
writes into `.claude/settings.json` and `.codex/hooks.json` stay out of this change; they follow in
a change of their own.
