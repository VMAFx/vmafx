<!-- markdownlint-disable MD013 MD060 -->
# ADR-0116: CI workflow naming convention — purpose-named files + Title Case display names

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, github, docs

## Context

The fork's six core CI workflows shipped with terse, single-word filenames and
matching lowercase `name:` fields (`ci.yml`, `lint.yml`, `security.yml`,
`libvmaf.yml`, `ffmpeg.yml`, `docker.yml`). On the Actions tab and in the
Checks API surface, "ci" and "libvmaf" do not communicate what they actually
do — `libvmaf` reads as "the library", not "the cross-platform build matrix",
and `ci` does not flag itself as the Netflix-golden / sanitizer / Tiny-AI /
coverage orchestrator. User direction: the workflow now called `libvmaf`
should make clear it is a matrix build test, not the library itself.

The required-status-check contexts in `master` branch protection
([ADR-0037](0037-master-branch-protection.md)) are bound to job-level `name:`
strings (matrix-expanded), so renaming any job-level name requires re-pinning
the protection contexts in the same change window. The 19 required contexts
are otherwise unchanged in semantics — only their human-readable display
strings move.

## Decision

We will use a uniform naming convention across all `.github/workflows/*.yml`:

1. **Filename** — purpose-descriptive kebab-case, `<area>-<scope>.yml`. The
   six core workflows rename as:
   - `ci.yml` → `tests-and-quality-gates.yml`
   - `lint.yml` → `lint-and-format.yml`
   - `security.yml` → `security-scans.yml`
   - `libvmaf.yml` → `libvmaf-build-matrix.yml`
   - `ffmpeg.yml` → `ffmpeg-integration.yml`
   - `docker.yml` → `docker-image.yml`
2. **Workflow `name:`** — Title Case, with an em-dash separating area from
   scope/axis list. Example:
   `Tests & Quality Gates — Netflix Golden / Sanitizers / Tiny AI / Coverage`.
3. **Job `name:`** (and matrix-expanded leg names) — Title Case sentences
   that name the gate's purpose plus axis tag. Examples: `Build — Ubuntu SYCL`,
   `Pre-Commit (Formatters + Basic Checks)`,
   `Coverage Gate (Ramping to 70% / 85% Critical)`.
4. **Status-check contexts** — driven by (3) above. After renaming, re-pin
   `required_status_checks.contexts` via
   `gh api --method PUT repos/VMAFx/vmafx/branches/master/protection` in
   the same merge window so protection does not break.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave names as they are | Zero churn; protection list stable | Actions UI / Checks API stays cryptic; new contributors must read each YAML to know what runs | User explicitly rejected — "the name of that workflow (libvmaf) should perhaps say/show that its just a matrix build test" |
| Rename only display `name:` fields, keep filenames | No URL changes; badges stay live | Filename and label diverge — when an issue references `ci.yml` you still have to translate to "Tests & Quality Gates" | Half-measure; readers grep filenames as much as labels |
| Rename only filenames, keep terse `name:` | URL update once, label stays | Display label stays cryptic where readers look first (Checks tab) | Same half-measure inverted |
| Rename both filenames and Title Case labels (chosen) | Filename and label both communicate purpose; one merge window of churn | Branch-protection re-pin required; README badges + skill scaffolds + a few doc references touch | User-approved sweep; the protection re-pin is one `gh api` call documented below |
| Add area prefixes (`ci-tests.yml`, `ci-lint.yml`, …) | Sortable by area | Redundant prefix when the area is already the most specific word | Rejected as noise |

## Consequences

- **Positive**: Actions UI and Checks API show purpose-named workflows and
  Title Case job labels; new contributors orient without opening each YAML;
  badge labels in README convey what each gate does at a glance.
- **Negative**: `master` branch protection's
  `required_status_checks.contexts` list must be re-pinned once after this PR
  merges (single `gh api` PUT). Any open PR or local branch that references
  the old filenames in CI badges or doc links will surface 404s until
  rebased.
- **Neutral / follow-ups**:
  - [ADR-0033](0033-codeql-config-moved-to-github.md)'s body references
    `.github/workflows/security.yml`; the file is now `security-scans.yml`
    but the decision (relocate the CodeQL config to `.github/`) stands.
    ADR bodies are immutable once Accepted ([ADR-0106](0106-adr-maintenance-rule.md)),
    so this ADR is the forward pointer.
  - [ADR-0037](0037-master-branch-protection.md)'s "19 required status
    checks" enumeration remains semantically accurate; the underlying
    context strings now use the Title Case names listed in §"Decision" (3).
    The `gh api` re-pin step in §"Consequences" is the operational
    follow-up.
  - The post-merge re-pin command is:

    ```text
    gh api --method PUT repos/VMAFx/vmafx/branches/master/protection \
      --input <updated-protection.json>
    ```

    where the JSON's `required_status_checks.contexts` array uses the new
    Title Case job names produced by this PR.
  - [`docs/principles.md`](../principles.md) line 5 updated from
    `{lint,security,supply-chain}.yml` to
    `{lint-and-format,security-scans,supply-chain}.yml`.
  - Skill scaffolds under `.claude/skills/add-gpu-backend/` updated to
    reference `libvmaf-build-matrix.yml` instead of `ci.yml`.
  - `README.md` badge URLs and labels updated to the new filenames + Title
    Case display strings.

## References

- Source: `req` (user direction, paraphrased: the libvmaf workflow's name
  should communicate that it is a build matrix test, not the library itself;
  approve all six rename + Title Case sweep + admin merge with immediate
  `gh api` re-pin)
- Related ADRs: [ADR-0033](0033-codeql-config-moved-to-github.md),
  [ADR-0037](0037-master-branch-protection.md),
  [ADR-0106](0106-adr-maintenance-rule.md),
  [ADR-0115](0115-ci-trigger-master-only-and-matrix-consolidation.md)
