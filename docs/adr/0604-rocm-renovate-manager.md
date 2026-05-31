<!-- markdownlint-disable MD060 -->
# ADR-0604: Add Renovate customManager for ROCm apt-repo tracking

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `build`, `container`, `supply-chain`, `hip`, `renovate`

## Context

The user directed a ROCm version bump in `dev/Containerfile` after observing
that ROCm 7.13 exists as AMD's current release. A live audit of
`repo.radeon.com/rocm/apt/` (2026-05-19) showed:

- The apt repo tops out at **7.2.3** — the same version already pinned.
- ROCm 7.13.0 is a **technology preview** distributed under the "TheRock"
  architecture, with no apt repo entry. AMD's own documentation states:
  "For production use, continue to use ROCm 7.2.3."

No version bump is therefore required. However, the audit exposed a gap in
automated dependency tracking: Renovate cannot detect new ROCm apt-repo
versions because the install uses AMD's custom apt channel
(`repo.radeon.com/rocm/apt/<version>`) which has no built-in Renovate
manager. Future stable releases would go unnoticed until a manual audit.

This ADR records the decision to add a Renovate `customManager` entry so
that when AMD eventually promotes a new production release to the apt repo,
Renovate opens a PR automatically.

## Decision

We will add a `customManagers` entry and a `customDatasources` block to
`renovate.json` that:

1. Matches the `ARG ROCM_VER=<version>` line and the `rocm/apt/<version>/`
   URL path in `dev/Containerfile` using `customType: "regex"`.
2. Queries the AMD apt directory listing at `https://repo.radeon.com/rocm/apt/`
   via `format: "html"` to detect new versions.
3. Groups ROCm bumps under a `packageRules` entry with
   `labels: ["dependencies", "rocm", "manual-review"]` and
   `automerge: false` — ROCm major-version bumps carry KFD ioctl ABI risk
   (as documented in ADR-0603) and require human validation before merging.

No version change is made to `dev/Containerfile`: 7.2.3 remains correct.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bump to ROCm 7.13.0 now | Tracks the preview | 7.13.0 has no apt packages; would require switching to the "TheRock" distribution channel, an untested install path, and a breaking KFD ABI risk on older kernels | Not yet production-stable; no apt repo entry |
| Leave Renovate unconfigured for ROCm | No change needed now | Future stable releases go undetected; manual audits required | Defeats the purpose of having Renovate |
| Use Renovate `regexManagers` on the apt URL only | Simpler | Only one match string; misses the `ARG ROCM_VER=` line and therefore may not update it consistently | Two match strings ensure both the ARG and the URL stay in sync |

## Consequences

- **Positive**: Future ROCm apt-repo releases (e.g. 7.2.4, 7.3.x, 8.x) will
  trigger an automatic Renovate PR with `manual-review` label, eliminating
  silent staleness.
- **Positive**: The Containerfile `ARG ROCM_VER` and the apt repo URL path
  are both covered, so they stay in sync across bumps.
- **Negative**: The `html` datasource depends on AMD maintaining the standard
  directory-listing format of `repo.radeon.com/rocm/apt/`. If AMD changes
  the listing structure, the regex match may silently stop working. A future
  audit check (or Renovate log review) is needed if ROCm PRs stop appearing.
- **Neutral**: ROCm 7.13.0 / "TheRock" preview channel requires a separate
  decision and Containerfile rework before it can be adopted. Tracked as a
  future follow-up once AMD publishes it as production-stable.

## References

- `req`: user direction "ROCm is fucking outdated btw. current version is 7.13"
  (paraphrased: the user identified ROCm as potentially outdated and requested
  a version bump to what they believed was the current release — 7.13)
- Live audit: `https://repo.radeon.com/rocm/apt/` (2026-05-19) — latest entry
  is 7.2.3
- AMD docs banner: `https://rocm.docs.amd.com/en/latest/` — "For production
  use, continue to use ROCm 7.2.3"
- [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) — Ubuntu 26.04 fallout,
  context for ROCm 7.2.3 + `noble` channel cross-install
- Research: `docs/research/rocm-version-audit-2026-05-19.md`
