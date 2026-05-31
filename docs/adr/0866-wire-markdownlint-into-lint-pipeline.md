# ADR-0866: Wire markdownlint-cli2 into make lint + pre-commit + CI

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: ci, docs, lint, hygiene

## Context

ADR-0864 (PR #332) tuned `.markdownlint.json` for the fork's prose-heavy
docs corpus — disabling 17 noisy or unsafe-to-autofix rules and running
`markdownlint-cli2 --fix` on the safe subset. After that sweep the
remaining warning tail across `docs/` + `changelog.d/` +
top-level markdown is ~6.2k (from a ~19.7k baseline).

ADR-0864 stopped at the config + cleanup. It did **not** wire the linter
into any gate:

- `make lint` calls only `clang-tidy`, `cppcheck`, `ruff`, `shellcheck`,
  and the fragment-tree drift check. No markdown step.
- `.pre-commit-config.yaml` has formatters and security hooks but no
  `markdownlint-cli2` hook.
- `.github/workflows/lint-and-format.yml` has `pre-commit`, `clang-tidy`,
  `cppcheck`, `python-lint`, `docs-lint` (mkdocs), `shellcheck` — no
  `markdownlint` job.

Without a gate, future PRs reintroduce the same classes of drift the
sweep just discharged. The touched-file lint-clean rule (CLAUDE.md §12
r12) only bites when a linter is actually wired up.

The pre-existing ~6.2k warnings forbid a naive all-files gate: every
docs-adjacent PR would inherit a wall of red from neighbours it didn't
touch. The only workable shape is the **touched-file** scope —
identical to how the C lint pipeline handles `clang-tidy` (delta
against the PR base ref, not the full tree).

## Decision

Wire `markdownlint-cli2` into all three surfaces, with **touched-file
scope** as the default so the pre-existing tail does not gate
PRs that don't touch it:

1. **`Makefile`** — add a `lint-md` phony target. Default scope is the
   delta against `origin/master` (touched markdown only). Override with
   `MDLINT_SCOPE=all` to run against the full corpus
   (`docs/**/*.md changelog.d/**/*.md README.md CLAUDE.md AGENTS.md`).
   `lint` depends on `lint-md`.

2. **`.pre-commit-config.yaml`** — add the official `markdownlint-cli2`
   repo hook (`https://github.com/DavidAnson/markdownlint-cli2`) with
   `pass_filenames: true` so it lints **only** staged markdown.
   **No `args: [--fix]`** — ADR-0864 documents 7 default rules that
   silently change meaning under autofix (MD004 prose conjunctions,
   MD018 PR-number headings, MD029 ordered-list renumbering,
   MD037/MD038 emphasis/code spacing, MD007 reference-list indent,
   MD027 license-block quoting).

3. **`.github/workflows/lint-and-format.yml`** — add a
   `markdownlint` job that mirrors the `clang-tidy` job's delta
   collection (PR / push / dispatch fan-out). Uses
   `actions/setup-node@v4` + `npx --yes markdownlint-cli2`. Runs the
   linter on the changed markdown set only; empty set short-circuits
   with success.

The wiring lands **after** PR #332. PR #332 ships the tuned config +
initial sweep (111 files); this PR ships only the gate around it.

**Auto-generated files and concat-script inputs excluded**:

- `docs/adr/README.md` + `CHANGELOG.md` — mechanically rendered from
  `docs/adr/_index_fragments/` and `changelog.d/` by
  `scripts/docs/concat-adr-index.sh` /
  `scripts/release/concat-changelog-fragments.sh` (ADR-0221).
  Linting the rendered output would surface pre-existing
  table-column-count warnings (MD056) in concatenated rows that
  cannot be fixed by editing the output.
- `docs/adr/_index_fragments/*.md` — each fragment is a bare table
  row (no first-line H1, single line > 80 cols by design).
- `changelog.d/<section>/*.md` — each fragment is body-only with a
  `## Section` heading (no first-line H1) per ADR-0221.

These fragments are pipeline inputs, not standalone documents. The
rendered output is excluded (auto-generated) and the inputs are
excluded (structurally trip MD013/MD041/MD056 by design). Sweep
PRs that touch the rendered output are unnecessary — the
concatenation is deterministic.

## Alternatives considered

<!-- markdownlint-disable MD013 MD060 -->
| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(Chosen) Touched-file scope by default, all-files via env opt-in** | Honours CLAUDE.md §12 r12 (touched-file lint-clean); innocent PRs don't inherit ~6.2k pre-existing tail; all-files mode still available for full audits | Two scope modes to document | Pragmatic — matches the existing `clang-tidy` job's delta-scoping pattern |
| All-files gate, fail on ~6.2k warnings | Maximally enforces the tuned config | Every docs-adjacent PR red on day 1; touched-file rule unsatisfiable | Adversarial; would block the merge train |
| All-files gate, allow-list-existing-warnings file | Forces zero-net-new warnings without ignoring debt | Allow-list file churns on every cleanup PR; merge-conflict factory | Maintenance burden outweighs the precision win |
| Advisory-only (`continue-on-error: true`) | No risk of false positives blocking PRs | No teeth — drift accrues silently; the entire reason to wire it in is to gate | Defeats the purpose |
| Skip wiring entirely, lean on PR review | Zero infra cost | Human review misses lint drift consistently; the ADR-0864 sweep was the proof | The status quo is what created the 19.7k baseline in the first place |
<!-- markdownlint-enable MD013 MD060 -->

## Consequences

- **Positive**:
  - Touched-file lint-clean rule (CLAUDE.md §12 r12) now extends to
    markdown without dragging in pre-existing warnings.
  - Future drift caught at the same gate that catches C/Python drift —
    same mental model for contributors.
  - `make lint-md MDLINT_SCOPE=all` remains available for periodic
    full-tree audits and follow-up sweep PRs.
- **Negative**:
  - One more job in `lint-and-format.yml` (~10 s).
  - Pre-existing ~6.2k warnings still live in the tree; only new
    drift on touched files is gated. Follow-up sweep PRs can chip
    away at the tail.
- **Neutral / follow-ups**:
  - PR #332 must land first (this PR's gate is meaningless against
    master's untuned config — it would report ~19.7k against any
    touched docs file).
  - Future sweep PRs that discharge tail warnings should append a
    `changelog.d/changed/markdown-lint-sweep-NNNN.md` fragment.

## References

- ADR-0864 — `.markdownlint.json` tune + initial sweep (PR #332).
- CLAUDE.md §12 rule 12 — touched-file lint-clean rule
  ([ADR-0141](0141-touched-file-cleanup-rule.md),
  [ADR-0278](0278-nolint-citation-closeout.md)).
- `markdownlint-cli2` upstream: <https://github.com/DavidAnson/markdownlint-cli2>.
- Related PRs: lands after #332.
- Source: `req` — direct user direction to wire the linter into
  `make lint` + pre-commit + CI after PR #332's sweep.
