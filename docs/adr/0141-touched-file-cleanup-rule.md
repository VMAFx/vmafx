# ADR-0141: Every PR leaves its touched files lint-clean

- **Status**: Accepted
- **Date**: 2026-04-21
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, process, code-quality, agents

## Context

The fork enforces a strict lint profile (NASA/JPL Power of 10 +
SEI CERT + clang-tidy `.clang-tidy`) on fork-added code. Upstream
Netflix code has historically been exempt, with suppressions and
legacy `// NOLINT` comments carried as-is on the grounds that
"we didn't write it". Over time this produced two cliffs:

1. A PR touching an upstream file to apply a one-line fix gets
   blamed for every pre-existing warning clang-tidy raises on
   the changed file. The PR author either (a) silences them
   with `// NOLINTNEXTLINE` everywhere (fast-but-dirty), (b)
   ignores them and merges with a red lint column (blind-spot
   risk), or (c) does a sweep-cleanup on the whole file (scope
   creep).
2. Follow-up work on the same file compounds the ambiguity —
   new code inherits the dirty baseline, and the "who cleans it"
   question keeps getting punted.

The specific trigger: PR #77 (SIMD DX framework, ADR-0140)
surfaced ~18 `readability-function-size` warnings across
`_iqa_convolve` / `_iqa_ssim` / `ssim_accumulate_{avx2,avx512,neon}`
/ test helpers. About half are in files the fork owns outright;
half are upstream Netflix code the fork has modified. The author
(Claude, this session) reached for blanket `// NOLINTNEXTLINE` —
which contradicts the fork's "defensive code, explicit
invariants" philosophy and is not sustainable for the next
twenty SIMD PRs.

## Decision

We will require that **every PR leaves every file it touches
lint-clean to the fork's strictest profile**, regardless of
whether the file is fork-local or upstream-mirror. "Touches"
means any hunk in the PR's diff against its merge base. The
rule applies retroactively the moment a PR first modifies a
previously-dirty file.

The fix precedence is explicit:

1. **Prefer a real refactor** — extract helpers, split functions,
   rename reserved identifiers, replace discarded return values
   with `(void)` casts, etc. This is the default.
2. **`// NOLINT` / `// NOLINTNEXTLINE` is reserved for cases
   where refactoring would break a load-bearing invariant** —
   e.g., an ADR-0138 / ADR-0139 bit-exactness pattern that
   requires an inline per-lane reduction, or an upstream-parity
   identifier the rebase story depends on keeping verbatim. Every
   NOLINT must cite, inline, the ADR / research digest / rebase
   invariant that forces it. A NOLINT without a justification
   comment is a lint violation in its own right.
3. **Historical debt** — the pre-2026-04-21 baseline of 18
   `readability-function-size` NOLINTs + `bugprone-reserved-identifier`
   / `cert-dcl37-c` suppressions on upstream `_iqa_*` code — is
   scoped to **backlog item T7-5** (see `.workingdir2/OPEN.md`),
   one sweep-PR gated by Netflix CPU golden + `/cross-backend-diff`.
   This ADR does not backdate the rule to force that sweep inside
   another in-flight PR.

The rule applies equally to fork-added code and upstream-mirror
code. The fork's lint policy is the fork's; if we touch the
file, we take ownership of leaving it clean. Rebase story: the
cleanup diff is listed in `docs/rebase-notes.md` for any upstream
file we refactor, with instructions to keep the fork's version
on conflict.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Touched-file cleanup required per-PR, refactor-first, NOLINT-only-when-justified (chosen)** | Debt stays bounded; next engineer starts from a clean baseline; upstream-mirror code gets pulled up to fork standards in the natural course of work | Short-term friction (PR authors can't ignore pre-existing warnings in a file they touch); some large-fn refactors (like `_iqa_convolve`) are out-of-scope for a PR making an unrelated change | **Decision** — per user direction 2026-04-21 |
| Fork-local code clean, upstream-mirror exempt | Low friction; matches historical practice | Two-class standard is brittle; upstream files tend to grow fork-specific changes over time and the exemption becomes a hiding place; contradicts "if we touch it, we own it" philosophy | Rejected — creates a silent tier-two of the codebase |
| Blanket NOLINT with justification, no refactor required | Fastest to green CI | Debt compounds silently; every NOLINT is a promise to revisit that rarely happens | Rejected — the fork already has 18 `readability-function-size` NOLINTs accumulated on this path |
| Global sweep-cleanup PR *first*, then enforce going forward | Clean baseline before the rule kicks in | Couples forward progress to a large sweep; contradicts T7-5's scope (sweep queued *after* current PR merges) | Rejected — sequence backwards; the rule is independent of the one-time sweep |

## Consequences

- **Positive**:
  - Every PR reviewer sees *only* warnings the PR introduced or
    chose not to fix — no more pre-existing noise drowning out
    the real signal.
  - Upstream-mirror files accrete fork-quality improvements
    naturally instead of via dedicated sweep PRs that compete
    for reviewer attention.
  - NOLINT becomes a load-bearing signal again: "this one
    genuinely can't be cleaned, here's why" — instead of "I was
    in a hurry".
- **Negative**:
  - PR authors pay a cleanup tax proportional to how dirty the
    touched file was. On very dirty files (`_iqa_ssim`,
    `_iqa_convolve`) the tax is non-trivial; authors will prefer
    to touch those files in the T7-5 sweep PR rather than
    opportunistically in an unrelated change.
  - Extra rebase surface when we refactor upstream code.
- **Neutral / follow-ups**:
  - Add a hard rule to [`CLAUDE.md`](../../CLAUDE.md) §12 pointing
    to this ADR.
  - Update [`AGENTS.md`](../../AGENTS.md) so non-Claude agents
    see the same rule.
  - T7-5 backlog item (`.workingdir2/OPEN.md`) stays; it's the
    one-time catch-up sweep for the 18 historical NOLINTs.
  - Each NOLINT going forward must cite an ADR / research digest /
    rebase invariant inline. A lint auditor script (future CI
    gate, out of scope for this ADR) can enforce the "NOLINT
    without a justification" sub-rule mechanically.

## References

- Source: user direction 2026-04-21
  (paraphrased: "the ADR should apply to everything from now
  on, not only PR #77" + "full lint/cleaning, NOLINT only if
  not possible otherwise").
- Historical-debt scoping:
  [`.workingdir2/OPEN.md`](../../.workingdir2/OPEN.md) T7-5 —
  one-time sweep of 18 `readability-function-size` NOLINTs +
  upstream `_iqa_*` suppressions, gated by Netflix golden +
  `/cross-backend-diff`, queued immediately after PR #76 /
  PR #77 merge to `master`.
- Related ADRs:
  [ADR-0108](0108-deep-dive-deliverables-rule.md) — deep-dive
  deliverables checklist (this rule joins the checklist);
  [ADR-0100](0100-project-wide-doc-substance-rule.md) — per-PR
  doc-substance rule (same shape: every PR ships its cleanup
  alongside the code).
- Trigger: PR [#77](https://github.com/VMAFx/vmafx/pull/77)
  surfaced 18 `readability-function-size` warnings plus
  14 `cert-err33-c` warnings on `mu_run_test` in the `test.h`
  macro. The `fprintf`-return-discard warnings were fixed at
  source (cast to `void`); the function-size warnings on
  `ssim_accumulate_{avx2,avx512,neon}` were refactored via a
  shared `static inline` per-lane helper; the `_iqa_convolve` /
  `_iqa_ssim` warnings were deferred to T7-5 with an inline
  `// NOLINT` + ADR citation.
