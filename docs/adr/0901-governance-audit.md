<!-- markdownlint-disable MD013 -->
# ADR-0901: Governance file audit — add GOVERNANCE + MAINTAINERS, expand CODEOWNERS, document ADR-0108 in CONTRIBUTING

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `governance`, `docs`, `meta`

## Context

A community-health audit of the fork's governance surfaces found two
present-and-stale and two missing artifacts:

1. **`.github/CODEOWNERS`** existed but only covered upstream-mirror
   subtrees (`/libvmaf/...`, `/python/...`) plus a few fork-local CI
   paths. Every Phase-3 / Phase-4 fork-local subtree added since —
   `/ai/`, `/mcp-server/`, `/cmd/` (Phase 4b distributed platform),
   `/deploy/`, `/docker/`, `/dev/`, `/compat/` (ADR-0700 rename
   target), `/ffmpeg-patches/`, `/scripts/`, `/docs/adr/`,
   `/changelog.d/` — had no explicit owner row. The default
   catch-all `* @Lusoris` did route reviews, but with no
   subtree-specific signal for future maintainer additions.
2. **`GOVERNANCE.md`** missing. The repo had no public statement of
   the governance model (BDFL), no description of how decisions are
   reached (ADRs), and no path for adding maintainers.
3. **`MAINTAINERS.md`** missing. The maintainer list was implicit
   ("everything goes through @Lusoris") with no documented
   onboarding / stepping-down process.
4. **`CONTRIBUTING.md`** existed and was substantive, but did not
   surface two policies a first-time contributor needs to ship a
   PR: branch naming conventions and the ADR-0108 six-deliverables
   gate. Both were buried in ADRs / `CLAUDE.md` / the PR template;
   external contributors looking only at `CONTRIBUTING.md` would
   miss them.

The other governance surfaces — `SECURITY.md`, `CODE_OF_CONDUCT.md`,
`.github/ISSUE_TEMPLATE/{bug_report,feature_request,performance_regression,config}.yml`,
`.github/PULL_REQUEST_TEMPLATE.md` — were already present, complete,
and aligned with the current fork policies; this audit leaves them
unchanged.

A concurrent PR (#321) is rewriting the existing CODEOWNERS
`/libvmaf/...` rows to their post-ADR-0700 `/core/...` equivalents.
This ADR is scoped to **additions only**; the rename of existing
rows is PR #321's territory and is not touched here to avoid a merge
conflict.

## Decision

We will:

- Add [`GOVERNANCE.md`](../../GOVERNANCE.md) documenting the BDFL
  model, the maintainer / contributor roles, the ADR-driven
  decision-making process, the upstream relationship, the release
  process, the security flow, and the amendment procedure.
- Add [`MAINTAINERS.md`](../../MAINTAINERS.md) listing the current
  BDFL (Lusoris), mapping subtrees to maintainers, and documenting
  the becoming-a-maintainer / stepping-down / inactive-maintainer
  processes.
- Extend [`.github/CODEOWNERS`](../../.github/CODEOWNERS) with
  per-subtree rows for every fork-local directory not currently
  covered (Tiny-AI, MCP servers, `/cmd/`, deployment, `/compat/`,
  `/ffmpeg-patches/`, `/scripts/`, `/tools/`, `/docs/adr/`,
  `/changelog.d/`) and for the governance files themselves.
  **Existing rows are left untouched** to avoid conflicting with
  in-flight PR #321.
- Update [`CONTRIBUTING.md`](../../CONTRIBUTING.md) to add three
  sections external contributors need:
  - **Branch naming** — Conventional-Commits-aligned prefixes.
  - **Deep-dive deliverables (ADR-0108)** — the six items required
    on every fork-local PR.
  - **Architecture Decision Records** — how to reserve a number via
    `scripts/adr/next-free.sh --claim`.
  - **Governance** — pointer to `GOVERNANCE.md` and `MAINTAINERS.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Do nothing — implicit governance | Zero diff, zero maintenance burden | Bad community-health signal; future maintainer onboarding has no documented path; external contributors miss ADR-0108 gate | Rejected — the gap is the point of the audit |
| Inline governance into `CLAUDE.md` instead of `GOVERNANCE.md` | One less file | `CLAUDE.md` is an agent-config file; humans (and GitHub's community-health UI) look for top-level `GOVERNANCE.md` | Rejected — wrong file for wrong audience |
| Wait for PR #321 to merge, then do CODEOWNERS in one pass | Single file diff | Blocks this PR on an unrelated path-rewrite; PR #321 has no governance scope | Rejected — additions are conflict-free with #321; deliver value now |
| Switch from BDFL to steering committee in the same PR | Models a more mature community | Premature — fork has one active maintainer; would be aspirational fiction | Rejected — document the actual model |
| Add a CLA-bot / DCO sign-off requirement | Stronger contribution provenance | Adds friction for casual contributors; upstream Netflix/vmaf doesn't require one | Rejected for now; revisit if contributor volume grows |

## Consequences

- **Positive**:
  - GitHub's community-health UI flips green for the missing
    artifacts.
  - First-time contributors discover the ADR-0108 gate and branch-
    naming convention from `CONTRIBUTING.md` directly, without
    having to read `CLAUDE.md` or 700+ ADRs.
  - CODEOWNERS now routes review for every fork-local subtree, so
    when a second maintainer joins the per-subtree rows can be
    edited in place instead of invented at that moment.
  - Clear written process for adding / removing maintainers; no
    bus-factor surprise.
- **Negative**:
  - One more pair of top-level files to keep current
    (`GOVERNANCE.md`, `MAINTAINERS.md`). Mitigated by both being
    short, declarative, and self-amendable via the normal ADR + PR
    flow.
  - CODEOWNERS grew by ~30 lines; one more place to maintain on
    subtree adds. Mitigated by the catch-all `* @Lusoris` rule
    already covering anything not explicitly listed.
- **Neutral / follow-ups**:
  - When PR #321 lands, the `/libvmaf/...` rows already in
    CODEOWNERS will become `/core/...`. This PR's additions live
    below those rows and require no further edit.
  - If a second maintainer joins, update both `MAINTAINERS.md` and
    the relevant CODEOWNERS rows in the same PR (called out in
    `MAINTAINERS.md` §"Becoming a maintainer").

## References

- Concurrent in-flight: PR #321 (CODEOWNERS path rewrite for
  ADR-0700) — explicit non-overlap.
- ADR-0024 — Netflix golden-data gate.
- ADR-0037 — Master branch protection.
- ADR-0100 — Project-wide doc-substance rule.
- ADR-0108 — Deep-dive deliverables rule.
- ADR-0141 — Touched-file lint-cleanup rule.
- ADR-0221 — Changelog / ADR fragment pattern.
- ADR-0535 — ADR atomic allocator.
- ADR-0628 — ADR allocator remote-aware.
- ADR-0700 — VMAFX repo layout (`libvmaf/` → `core/`).
- Source: `req` — task brief from the user requesting a governance-file
  audit, dispatched 2026-05-30.
