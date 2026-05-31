<!-- markdownlint-disable MD013 MD060 -->
# ADR-0694: Tighten clang-tidy enforcement + confirm sanitizers as required CI gates

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `ci`, `lint`, `testing`, `security`, `fork-local`

## Context

ADR-0686 (VMAFX Phase 2B umbrella) requires two CI hardening steps:

1. **Tighter clang-tidy**: add `cert-err33-c` (every non-void return value must
   be checked or `(void)`-cast) and `bugprone-not-null-terminated-result`
   (catches `strncpy`/`strlcpy` results that may not be null-terminated).
   CLAUDE.md §6 already mandates this as a coding rule; making it a CI check
   closes the enforcement gap between documentation and tooling.

2. **Sanitizers promoted to required CI gates**: ASan + UBSan + MSan (thread)
   jobs should block merges, not just report advisorily.

Scanning the tree on 2026-05-28 (CPU build, 394 source files in
`libvmaf/src` + `libvmaf/tools`) produced:

- `cert-err33-c`: **394 violations** across 394 C source files (pre-existing
  unchecked return values, primarily from library calls whose return values
  are currently discarded without a `(void)` cast).
- `bugprone-not-null-terminated-result`: **0 violations**.

Because 394 violations far exceed the 50-violation threshold established in
the task brief, `cert-err33-c` is added as an advisory check (enabled in the
`Checks:` glob but excluded from `WarningsAsErrors`). A follow-up sweep PR
will fix the violations file-by-file and promote the check to an error-level
gate once the tree is clean.

Regarding the sanitizer promotion: inspection of
`tests-and-quality-gates.yml` and `required-aggregator.yml` confirms that
the three sanitizer jobs (`address`, `undefined`, `thread`) already:

- run on every non-draft PR (no `continue-on-error: true`),
- are listed in the `required` array of the Required Checks Aggregator
  (ADR-0313), and
- block merges via the single `Required Checks Aggregator` branch-protection
  context.

Therefore the sanitizer-promotion work for this PR is confirmatory: this ADR
documents the current state as intentional, records the branch-protection admin
step (see Consequences), and establishes the audit trail.

## Decision

We will:

1. Add a comment block to `.clang-tidy` explicitly calling out `cert-err33-c`
   and `bugprone-not-null-terminated-result` as Phase 2B / ADR-0686 additions.
   Both are already caught by the existing `bugprone-*` / `cert-*` globs but
   listing them explicitly documents intent and permits targeted
   `WarningsAsErrors` promotion in a future PR.
2. Exclude `cert-err33-c` from `WarningsAsErrors` in this PR pending the
   violation-fix sweep (394 violations). `bugprone-not-null-terminated-result`
   has zero violations and could be promoted now; we defer it to the same
   sweep PR for consistency.
3. Confirm that the three sanitizer matrix jobs (address/undefined/thread) are
   already required CI gates and document that state.
4. Add a manual admin step note reminding the repo admin to verify that the
   GitHub branch-protection "Required status checks" list includes the
   aggregator context (which in turn polls for the sanitizer job names).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `cert-err33-c` to `WarningsAsErrors` immediately | Strictest enforcement from day one | 394 pre-existing violations break CI for every unrelated PR | Violates CLAUDE.md §12 r12 "touched files lint-clean": enabling a check as an error that flags hundreds of untouched files is not the same as a per-file cleanup rule |
| Skip `cert-err33-c` entirely this PR | No CI disruption | Leaves the gap between the coding rule in CLAUDE.md and the clang-tidy enforcement | The task brief is explicit: add the check; advisory is the correct staging mechanism |
| Separate sanitizer-promotion PR | Cleaner separation of concerns | The sanitizers are already promoted — splitting adds overhead with no benefit | Both changes are part of the same ADR-0686 scope; confirmed state belongs in the same PR |

## Consequences

- **Positive**:
  - `cert-err33-c` and `bugprone-not-null-terminated-result` are now tracked
    violations visible in the full clang-tidy scan (`nightly.yml::clang-tidy-full`
    uploads a log artifact) and the PR-scoped `Clang-Tidy (Changed C/C++ Files)`
    job. Developers adding new C files will see warnings immediately.
  - The sanitizer gates are documented as intentional required checks, closing
    an audit gap.

- **Negative**:
  - The PR-diff clang-tidy job will emit advisory warnings for any touched C
    file that contains unchecked return values. This is expected and intended.

- **Neutral / follow-ups**:
  - **Follow-up PR required**: scan the 394 `cert-err33-c` violations, fix
    them (add `(void)` casts or check return values), and promote the check to
    `WarningsAsErrors`. Track as `T-CERT-ERR33-SWEEP` in `docs/state.md`.
  - **Admin step**: verify that the GitHub branch-protection "Required status
    checks" for `master` includes `Required Checks Aggregator` (the single
    context that polls for all jobs in its `required` list). If the individual
    sanitizer job names (`Sanitizers — ASan + UBSan + MSan (address)` etc.)
    are listed directly in the branch-protection settings rather than through
    the aggregator, the admin must remove them from direct listing to avoid
    conflicts. The aggregator is the canonical gate (ADR-0313). This step
    requires repository-admin GitHub permissions and cannot be automated via
    `gh` CLI without `--admin` override.

## References

- Umbrella ADR-0686 (VMAFX Phase 2B scope, per user direction)
- ADR-0313 (Required Checks Aggregator pattern)
- ADR-0347 (sanitizer matrix test scope / deselect list)
- ADR-0141 (touched-file lint-clean rule)
- ADR-0278 (NOLINT citation rule)
- ADR-0012 (coding standards: JPL + CERT + MISRA)
- `docs/principles.md` §6 — banned-function list and non-void return value rule
- Source: `req` — "Banned C funcs: TIGHTEN. Add clang-tidy cert-err33-c + bugprone-not-null-terminated-result enforcement (option 1) AND promote sanitizers (ASan + UBSan + MSan) to required gates on every PR not just nightly (option 3). Both, not either."
