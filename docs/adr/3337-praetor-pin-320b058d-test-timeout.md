<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3337: Move the praetor pin to 320b058d and excuse the Coverage Gate timeouts for three days

- **Status**: Accepted
- **Date**: 2026-10-17
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

Praetor `320b058d8cca` changes text that `praetorctl adopt` ships unchanged
here and adds one audit rule:

- `praetor-api.yml` and `praetor-docs.yml` gain a `concurrency` group and
  check out with `persist-credentials: false`.
- `.config/agent/hooks/block_evasion.py` matches `dev\` as well as `dev/` in
  the scaffold rule and searches the git option-run rule in linear time
  (cordanaLLM/praetor#1283).
- `tools/apicompat/gate/main.go` takes praetor's current text.
- The figure player bundle moves from `tools/figures/dist/` to
  `tools/figures/player/`, and the managed `.gitignore` block lists the
  private state roots unanchored plus `/.claude/settings.local.json` and
  `/.standards-receipt.json`.
- `forge.AuditTestTimeouts` (cordanaLLM/praetor#1381) fails every workflow
  timeout above 30 minutes. `tests-and-quality-gates.yml` sets 55m (line
  1230), 70m (line 1258) and 55m (line 1262) for the Coverage Gate, which runs
  the whole instrumented pytest suite in one step, measured at 37 to 51
  minutes.

## Decision

- Run `praetorctl adopt` (no `--force`) at `320b058d` and copy back only the
  files audit reports stale and the files praetor ships unchanged; move
  `PRAETOR_REF` to `320b058d8cca48eb7ce29c67b19e2b0a1f1dd917` under
  [ADR-1351](1351-praetor-engine-pin-move.md)'s conditions.
- Point the repository's own references to the player at
  `tools/figures/player/` (`REUSE.toml`, `docs/credits.yaml`, the relicense
  test) and drop the `!/tools/figures/dist/` re-include from `.gitignore`,
  which the new path does not need.
- Excuse the three Coverage Gate timeouts with one `test-timeout` exception
  in `.config/lint-exceptions.d/test-timeout.toml`, category B: it expires on
  2026-10-20. `scripts/ci/praetor_tidy_coverage.py` copies the entry into
  `.standards.yaml`, as it copies the HISS-10 and HISS-11 entries.
- The next unit splits the Coverage Gate pytest into steps of at most 30
  minutes and deletes the exception before it expires.
- Not copied back, as at the earlier pin moves: the generic `AGENTS.md`, the
  `praetorctl hook` pre-tool entries in `.claude/settings.json`,
  `.codex/hooks.json` and `.gemini/settings.json` (a change of its own, ledger
  `vmafx-adopt-pretool-hook`), and `.idea`.

## Alternatives considered

| Option | Why not |
|---|---|
| Split the Coverage Gate in this change | It changes what the gate measures and needs its own coverage-merge proof; mixing it into the pin move makes both harder to review. The exception buys three days for it. |
| Lower the timeouts to 30m | The suite takes 37 to 51 minutes; the gate would time out instead of measuring. |
| An unscoped or long-lived exception | The ledger answer narrows it to one file, three lines and category B, so the debt cannot grow silently. |
| Write the exception straight into `.standards.yaml` | That block is generated; a hand entry is lost at the next `--write`. |

## Consequences

- **Positive**: the gate workflows run with the engine's hardened text, and
  the adopter hook no longer goes quadratic on long git command lines.
- **Neutral**: `praetorctl audit` at `320b058d` exits 0 with the exception and
  1 without it, naming the three lines (measured for this change).
- **Negative**: the Coverage Gate keeps a timeout above the bound until the
  split lands; the expiry fails the audit if it does not.
- **Follow-ups**: the Coverage Gate split; the pre-tool guard
  (`vmafx-adopt-pretool-hook`).

## References

- Controller relay and go of 2026-10-17 (paraphrased): move the engine to
  `320b058d`, run `adopt` without `--force`, land one pull request at a clean
  merge state.
- Ledger `vmafx-engine-320b058d-test-timeout`, answer a (paraphrased): a
  scoped test-timeout exception for the three lines only, category B, citing
  cordanaLLM/praetor#1381 and the measured run time; the split follows.
- cordanaLLM/praetor#1381, #1283.
