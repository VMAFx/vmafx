<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3272: Move the praetor pin to 9bf502ec6ce5 and run the hooks through its engine launcher

- **Status**: Accepted
- **Date**: 2026-10-12
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

The governance hooks ran whatever `praetorctl` was first on `PATH`. CI installs
the engine pinned as `PRAETOR_REF` in `.github/workflows/standards-gate.yml`, so
the two could differ, and on 2026-10-11 they did: the workstation's engine had
moved ahead of the pin and refused every commit
([ADR-3240](3240-praetor-pin-4ae6247c.md),
`T-HOOKS-REFUSE-COMMITS-ENGINE-4AE6247C-2026-10-11`). ADR-3240 moved the pin to
that engine and left the cause open as `T-HOOKS-ENGINE-FROM-PATH-2026-10-11`:
praetor's launcher for the pinned engine was not written for a repository that
keeps its own `lefthook.yml` (cordanaLLM/praetor#1172).

That gap is closed three praetor commits after `4ae6247c57d2`:

| Praetor commit | What it brings here |
|---|---|
| `7cf9cbb2d` (cordanaLLM/praetor#1229) | `adopt` installs the launcher `.config/lefthook/engine.sh` and `.config/praetor/engine.mk` beside a kept `lefthook.yml`. |
| `299d3bebd` (cordanaLLM/praetor#1252) | Scanner and lint findings cleared in the files praetor ships. |
| `9bf502ec6` (cordanaLLM/praetor#1257) | The rest of those findings: a complete character-class escape and an unused binding. |

The launcher reads the pin from the workflows, installs that engine once per
pin into the user cache with `go install`, and runs it. A pin it cannot read,
two different pins, or a failed install end the hook; it never falls back to
`PATH`. With no pin declared at all it runs the `PATH` engine and says so on
standard error.

`praetorctl audit --offline` reports the same 143 warnings with both engines,
line for line: 142 "Workflow triggers" and one about cache-band markers in
`AGENTS.md`. The old engine was measured on a clean clone at the branch base,
the new one through the launcher on this tree.

## Decision

Move `PRAETOR_REF` to `9bf502ec6ce585110f37bf7c9e08d854d957d550` under
[ADR-1351](1351-praetor-engine-pin-move.md)'s conditions and run every hook's
engine call through the launcher.

- **Launcher adopted.** `.config/lefthook/engine.sh` and
  `.config/praetor/engine.mk` are praetor's bytes, taken from
  `praetorctl adopt --lock-source-root=<praetor clone at the pin>` in a
  throwaway clone without `--force`. Neither is edited by hand.
- **Hooks call it.** The seven governance jobs of `lefthook.yml`,
  `scripts/git-hooks/hiss-audit.sh` and `scripts/githooks/state-sync.sh` run
  `sh .config/lefthook/engine.sh <arguments>`. The `PATH` and `go run`
  fallbacks they carried are removed, so a missing launcher fails the hook
  instead of reaching another engine.
- **Refreshed praetor files.** `tools/figures/checks.mjs`,
  `tools/markdownlint/verify.mjs`, `tools/markdownlint/no-private-scratch-links.mjs`
  and `.config/agent/hooks/block_evasion.py` are praetor's text at the pin.
- **Formatter exception, declared.** The launcher is not stable under
  `shfmt -i 2 -ci`. `praetorctl audit` does not read the launcher; `adopt`
  refreshes it at a later pin only while it is one of praetor's own texts and
  keeps an edited copy as the repository's file, `--force` included. A
  formatted launcher would stop following the pin. It is one dated
  entry in `.config/lint-exceptions.d/shfmt.toml` (expiry 2026-12-31,
  cordanaLLM/praetor#1282). The `shfmt-src` hook, the `SHFMT_FILES` list of the
  `Makefile` and the native pre-commit hook skip exactly the listed files, and
  `scripts/ci/tests/test_shfmt_scope.py` holds the three to the list.
- **Licence line exception, declared.** The launcher carries no SPDX line
  (cordanaLLM/praetor#1281) and takes none here, for the same reason; it is
  one dated entry in
  `.config/lint-exceptions.d/spdx.toml` and `REUSE.toml`'s default covers it.
- **Stale exception removed.** `block_evasion.py` now passes ruff as praetor
  ships it, so its entry in `.config/lint-exceptions.d/ruff.toml` and the
  matching excludes are gone.

Not copied back from the throwaway clone: `.claude/settings.json`,
`.codex/hooks.json` and `.gemini/settings.json`. The pre-tool guard `adopt`
registers there lands in its own change.

## Alternatives considered

| Option | Why not |
|---|---|
| Keep the pin and the `PATH` engine | The workstation engine moves several times a day. Each move can refuse an unchanged tree again until the pin follows. |
| A repository-local wrapper that installs the pinned engine | A second implementation of what praetor now ships and refreshes; it would drift from the launcher other repositories run. |
| Adopt at `299d3bebd` | It still carries the shipped-file findings `9bf502ec6` clears, which the scanners here would report on praetor's bytes. |
| Format the launcher with shfmt | `adopt` keeps an edited launcher as the repository's own file and no longer refreshes it, so it would fall behind praetor's text at the next pin move. The fix belongs in praetor (cordanaLLM/praetor#1282). |
| Include `engine.mk` in the root `Makefile` now | The governance targets there call `standardsctl` from `PATH` (cordanaLLM/praetor#1231). Changing what `make verify-all` runs is a separate measured step. |

## Consequences

- **Positive**: a commit or push is judged by the engine CI installs, whatever
  is first on `PATH`. `T-HOOKS-ENGINE-FROM-PATH-2026-10-11` closes. A pin move
  no longer needs a `PATH` switch on every workstation.
- **Positive**: the hooks fail closed. An unusable pin or two different pins
  end the audit hook and the state sync without an engine run
  (`scripts/git-hooks/test-hiss-audit-offline.py`,
  `scripts/githooks/tests/test_install.py`). With the launcher file taken out,
  the audit hook ended with status 127 and the `PATH` engine was not called.
- **Negative**: the first hook after a pin move installs the engine, which
  needs the network and a Go toolchain; the install is bounded by
  `PRAETOR_ENGINE_INSTALL_TIMEOUT` (300 seconds by default).
- **Negative**: two dated exceptions exist for one praetor-owned file until
  praetor ships it formatted and with a licence line.
- **Neutral / follow-ups**: the agent hooks still run the `PATH` engine
  (cordanaLLM/praetor#1173), a direct `praetorctl` call with a differing pin
  still writes without a notice (cordanaLLM/praetor#1194), and the `Makefile`
  governance targets still call `standardsctl` from `PATH`
  (cordanaLLM/praetor#1231).

## References

- Source: operator decision of 2026-10-11, ledger id
  `vmafx-launcher-adoption-commit` (paraphrased): adopt at the newest engine
  main when the unit starts, the merge of cordanaLLM/praetor#1257 if it has
  landed, never an older commit.
- Source: operator decision of 2026-10-11, ledger id `vmafx-adopt-pretool-hook`
  (paraphrased): the pre-tool guard is adopted in a follow-up change of its
  own, after this one.
- cordanaLLM/praetor#1172, #1229, #1252, #1257 (the launcher beside a kept
  `lefthook.yml` and the cleared findings); #906 and #1165 (the launcher's
  design); #1173, #1194, #1231 (open follow-ups); #1281, #1282 (the two
  exceptions).
- [ADR-1351](1351-praetor-engine-pin-move.md) (conditions of a pin move),
  [ADR-3240](3240-praetor-pin-4ae6247c.md) (the previous pin).
