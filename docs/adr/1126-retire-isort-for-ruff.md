<!-- markdownlint-disable MD013 -->

# ADR-1126: Retire the standalone isort hook; ruff's `I` rules own import sorting

- **Status**: Accepted
- **Date**: 2026-08-30
- **Deciders**: Lusoris
- **Tags**: `ci`, `build`, `docs`

## Context

The repository ran **two** import sorters over the same files: the
standalone `isort` pre-commit hook (pinned `9.0.1`) and ruff's `I`
ruleset, which is selected in `[tool.ruff.lint]` and implements isort.
Their configurations were kept deliberately in sync — the
`[tool.ruff.lint.isort]` block carries the comment *"Must mirror
`[tool.isort].known_first_party` so isort and ruff-check agree"* — but
mirroring the first-party list is not sufficient to make two independent
implementations agree on every input.

Two inputs on `master` made them disagree outright, and because both
hooks auto-fix, `pre-commit run --all-files` never reached a fixed
point: isort rewrote a file, ruff rewrote it back, and the run reported
failure with a net-zero `git diff`. That is the
`Pre-Commit (Formatters + Basic Checks)` required check failing on
`master`, and its net-zero diff is why it was easy to misread as a
flake.

1. **Per-member trailing comments.** For
   `from vmaf_train.op_allowlist import (check_model,  # type: ignore[...])`
   isort collapses the statement onto one line (89 chars, under the
   shared 100-char limit) while ruff re-splits it to keep the comment
   attached to the member.
2. **Alias ordering.** Given both `from X import (a, b)` and
   `from X import y as z`, isort orders the alias statement first and
   ruff orders it second.

Neither is a misconfiguration that mirroring can fix; they are genuine
behavioural differences between two implementations.

## Decision

We will remove the standalone `isort` pre-commit hook, the `isort`
invocation in the `Python Lint` CI job, and the `[tool.isort]` section
from `pyproject.toml`. Ruff's `I` ruleset — already enabled and already
enforcing `I001` — becomes the single source of truth for import
ordering.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Retire isort, keep ruff `I`** (chosen) | One implementation, so the class of conflict cannot recur; zero file churn — verified that ruff + black already converge on `master` with an empty diff; ruff is strictly faster | Loses isort-specific settings nobody was relying on | — |
| Retire ruff `I`, keep isort | Also removes the conflict | Keeps the slower, single-purpose tool and drops a rule from the linter the project otherwise standardises on; contrary to consolidating on ruff | Wrong direction |
| Set `combine_as_imports` in both configs | Looked like the minimal config fix | **Measured**: churns 8 files and *still* oscillates — it addresses the alias-ordering case but not the trailing-comment case | Does not actually fix it |
| Reshape the two offending imports, keep both tools | Smallest diff | Treats the symptom; the next import with a per-member comment reintroduces the deadlock, and nothing warns the author | Leaves the trap armed |
| `# isort: skip` on the offending lines | Very small diff | Same as above, plus permanent unexplained pragmas | Leaves the trap armed |

## Consequences

- **Positive**: `pre-commit run --all-files` reaches a fixed point
  again, unblocking the `Pre-Commit` required check. One import sorter
  means no further mirroring burden between two config blocks. Ruff
  replaces a separate hook environment, so pre-commit gets faster.
- **Negative**: contributors with muscle-memory `isort .` invocations
  must use `ruff check --fix` instead. Import ordering may differ in
  rare corners from what isort produced; ruff's ordering is now
  definitionally correct for this repo.
- **Neutral / follow-ups**: no source file changes are required —
  `master` is already ruff-clean, so this lands as a
  configuration-only change. `docs/development/` guidance that names
  isort is updated in the same PR.

## Supply-chain impact

- **Removed dependencies**: `isort` (`dev`, pinned `9.0.1`) as a
  pre-commit hook environment and as a `Python Lint` CI step. The
  removal is complete for the enforcement path; `isort` may still be
  pulled in transitively by unrelated dev tooling, which is harmless
  because nothing invokes it.

## References

- req: the user asked to get master green and the PRs merged; this is
  the root cause of the `Pre-Commit` required check failing.
- Measured evidence: on a clean `origin/master` worktree, `isort`
  rewrites `tools/vmaf-tune/src/vmaftune/predictor_train.py` and
  `tools/vmaf-tune/tests/test_codec_adapter_av1_videotoolbox.py`, and
  `ruff-check` rewrites both back; three consecutive `ruff` + `black`
  rounds with isort absent produce an empty `git status`.
