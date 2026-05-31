<!-- markdownlint-disable MD060 -->
# ADR-0905: `.gitignore` + `.github/workflows/` staleness audit (2026-05-30)

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris (via agent brief)
- **Tags**: `repo-hygiene`, `ci`, `docs`

## Context

Two events left untrimmed rules in the repository's ignore /
automation footprint:

1. **ADR-0700** renamed `libvmaf/` → `core/` and
   `python/vmaf/` → `compat/python-vmaf/`. Most consumers were
   updated, but `.gitignore` and `python/.gitignore` still pin
   pre-rename paths in several rule families (MATLAB MEX
   blocklist, legacy test-resource scopes, Cython output).
2. The rebrand-driven workflow expansion on 2026-05-30 (PR #242)
   added five new release- and cron-triggered workflows; they
   have no run history yet, which superficially looks like dead
   automation but is dormancy by design.

In parallel, two other PRs already touch `.gitignore`: PR #321
rewires `libvmaf/subprojects/` → `core/subprojects/`, and
PR #330 anchors the Go binary roots with leading-slash patterns.
This audit must avoid colliding with either.

## Decision

Trim only the rules that (a) have no matching artefact in the
tree, (b) have no documentation comment justifying them as a
forward-looking guard, and (c) are not under active rewrite by
another open PR. Add rules where a relocated source still
produces an ignored artefact (the Cython `.c` output from the
relocated `adm_dwt2_cy.pyx`). Leave every workflow file in place;
the five no-runs workflows are correctly dormant.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Aggressive sweep — also drop `.idea/`, `vmaf_output.xml`, `preprints*.pdf`, `*.mkv` | Maximum shrink. | Removes well-known dev-tooling defenses; future contributors who use JetBrains / produce one-off CLI XML output regain a foot-gun. | Conservative bar matches the task brief ("documented pattern: keep"). |
| Remove the five no-runs workflows (`supply-chain.yml`, four `upstream-*-watcher.yml`) | Smaller workflow set. | All five are legitimate dormant automation (release-triggered or freshly added cron). Removing them silently disables the ADR-0448 upstream-watcher contract. | Keep — they will fire on schedule. |
| Hold the audit until PR #321 and PR #330 merge | No diff-overlap risk. | Stale paths persist for the in-flight ADR-0700 rename window; the agent task explicitly carves around those two PRs. | Carve-around is mechanical and verified by reading their diffs. |

## Consequences

- **Positive**: `.gitignore` reflects the post-ADR-0700 layout;
  the Cython-generated `.c` is once again ignored at its real
  location; `python/.gitignore` no longer maps to phantom dirs.
- **Negative**: Three independent PRs (this one, #321, #330) all
  modify `.gitignore`; the merge order is fixed by the carve-out
  but mid-flight rebases need attention.
- **Neutral / follow-ups**: The five new upstream-watcher
  workflows must produce at least one run within a week — if not,
  a follow-up ADR should investigate cron-schedule wiring. No new
  rules added beyond the two Cython output paths.

## References

- See [ADR-0700](0700-vmafx-repo-layout.md) for the rename that
  obsoleted the legacy paths.
- See [ADR-0448](0448-active-upstream-monitoring-discipline.md)
  for the upstream-watcher pattern that justifies the no-runs-yet
  workflows.
- See [ADR-0108](0108-deep-dive-deliverables-rule.md) for the
  six-deliverables contract.
- Research digest: [`docs/research/0905-gitignore-and-workflow-audit.md`](../research/0905-gitignore-and-workflow-audit.md).
- Related in-flight PRs (carve-out): #321, #330.
- Workflow-addition PR: #242.
- Source: agent task brief from lusoris (paraphrased: audit
  `.gitignore` files for stale rules and `.github/workflows/` for
  never-triggered workflows, avoid PR #321 and PR #330 overlap).
