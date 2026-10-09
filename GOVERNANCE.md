# Governance

This document describes how the VMAFX fork (formerly the `lusoris/vmaf`
fork of [Netflix/vmaf](https://github.com/Netflix/vmaf)) is governed.
For the upstream Netflix project, see
[Netflix/vmaf's governance](https://github.com/Netflix/vmaf/).

## 1. Project scope

VMAFX is a fork of Netflix/vmaf that adds:

- GPU backends — SYCL, CUDA, HIP, and Metal.
- SIMD paths — AVX2, AVX-512, NEON.
- Tiny-AI ONNX Runtime integration.
- Embedded and standalone MCP servers.
- Production / cloud-native distribution surfaces — Helm chart,
  Operator skeleton, controller / node split (Phase 4b, see
  ADR-0709).

Numerical correctness on the three Netflix CPU golden pairs (see
[ADR-0024](docs/adr/0024-netflix-golden-preserved.md) and `CLAUDE.md`
§8) is non-negotiable; the fork preserves upstream behavior on those
inputs as a binding constraint.

## 2. Roles

### 2.1 Benevolent Dictator (BDFL)

The fork is currently maintained under a **BDFL** model. The BDFL has
final say on:

- Architectural decisions captured as ADRs under
  [`docs/adr/`](docs/adr/).
- Acceptance of code into `master`.
- Release cadence and independent SemVer policy (`vX.Y.Z` — see
  [ADR-1127](docs/adr/1127-single-semver-release-stream.md)).
- Security advisories and coordinated disclosure.

The current BDFL is listed in [`MAINTAINERS.md`](MAINTAINERS.md).

### 2.2 Maintainers

Maintainers have **write access** and CODEOWNERS responsibility for
specific subtrees (see [`.github/CODEOWNERS`](.github/CODEOWNERS)).
Maintainers are listed in [`MAINTAINERS.md`](MAINTAINERS.md) with the
subtrees they own.

A maintainer is responsible for:

- Reviewing PRs that touch their owned subtree.
- Keeping the subtree's `AGENTS.md` (per-package invariants) and
  documentation in sync with the code (per `CLAUDE.md` §12 r10).
- Triaging issues filed against their subtree.

### 2.3 Contributors

Anyone who opens an issue or PR is a contributor. Contributors do not
need to sign a CLA. Each commit carries a Developer Certificate of Origin
sign-off ([ADR-2462](docs/adr/2462-dco-sign-off-required.md)), and by
submitting code they agree to license that contribution under the licence
governing the file they touch: EUPL-1.2
for new and fork-authored files, and the inherited licence for files
that carry someone else's code (see [`LICENSE`](LICENSE) for the EUPL-1.2,
[`NOTICE`](NOTICE) for Netflix's terms,
[`CONTRIBUTING.md`](CONTRIBUTING.md) and
[ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).

## 3. Decision-making

### 3.1 Architectural decisions — ADRs

Every non-trivial architectural, policy, or scope decision lands as
an Architecture Decision Record under
[`docs/adr/`](docs/adr/) **before** the implementing commit. Per
`CLAUDE.md` §12 r8, non-trivial means another engineer could
reasonably have chosen differently — directory moves, base-image
policy, CI-gate semantics, test-selection rules, new dependencies,
coding-standards changes.

ADRs follow [Michael Nygard's template](docs/adr/0000-template.md):
Status / Context / Decision / Alternatives considered /
Consequences / References. Once an ADR's Status flips to **Accepted**,
its body is **immutable** — superseding decisions get a new ADR that
links back via `Supersedes`.

To reserve an ADR number atomically — including across worktrees and
remote in-flight branches — run:

```bash
scripts/adr/next-free.sh --claim <kebab-slug>
```

See [ADR-0628](docs/adr/0628-adr-allocator-remote-aware.md) for the
allocator semantics.

### 3.2 Routine changes — PRs

Bug fixes and implementation work flow through pull requests against
`master`. Every PR must satisfy:

- Conventional Commits (`type(scope): subject`) — enforced by the
  `commit-msg` hook.
- The Netflix golden-data gate ([ADR-0024](docs/adr/0024-netflix-golden-preserved.md)).
- The deep-dive deliverables checklist
  ([ADR-0108](docs/adr/0108-deep-dive-deliverables-rule.md)) for
  fork-local PRs.
- The touched-file lint-cleanup rule
  ([ADR-0141](docs/adr/0141-touched-file-cleanup-rule.md)).
- The doc-substance rule
  ([ADR-0100](docs/adr/0100-project-wide-doc-substance-rule.md)).

`master` is host-protected — no force-push, no direct commits,
linear history required, 23 required status checks (see
[ADR-0037](docs/adr/0037-master-branch-protection.md)).

### 3.3 Disagreements

Reasonable disagreements about an ADR or PR are resolved in the
PR / ADR thread. Where consensus is not reached, the BDFL decides
and the rationale is captured in the ADR's `## References` section.

## 4. Upstream relationship

VMAFX is a hard fork — the histories no longer share a merge base in
the conventional sense. Synchronization with upstream Netflix/vmaf
happens via:

- `/sync-upstream` — periodic reconciliation, port-only topology
  (see [`docs/development/release.md`](docs/development/release.md)
  and the `sync-upstream` skill).
- `/port-upstream-commit <sha>` — single-commit cherry-picks for
  individual upstream fixes.

Upstream-port PRs are **exempt** from the ADR-0108 deliverables
checklist; everything else is fork-local and goes through the full
gate.

## 5. Releases

Releases are automated by `release-please` on pushes to `master`.
The version scheme is ordinary `vX.Y.Z` SemVer. VMAFx advances that stream
independently; upstream Netflix/vmaf provenance is recorded in release notes
and Git history rather than encoded in the tag. See
[ADR-1127](docs/adr/1127-single-semver-release-stream.md).

Every tagged release ships:

- SBOM (SPDX + CycloneDX).
- Sigstore keyless signatures (`cosign verify-blob ...`).
- GitHub build-provenance attestations with the SLSA v1 provenance predicate
  (`gh attestation verify ...`,
  [ADR-1356](docs/adr/1356-release-provenance-attest.md)).

See [`SECURITY.md`](SECURITY.md) §"Supply-chain guarantees" and
[ADR-0010](docs/adr/0010-sigstore-keyless-signing.md). Local
dry-runs go through `/prep-release` before a release PR is merged.

## 6. Security

Vulnerability reports follow the coordinated-disclosure flow in
[`SECURITY.md`](SECURITY.md). Public issues are **not** the right
channel for security problems; use the GitHub private
vulnerability-reporting form or the alternative email channel listed
there.

## 7. Code of Conduct

All community interactions are governed by
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md), adapted from the
Contributor Covenant v2.1. Enforcement is the responsibility of
maintainers; reports go to the address listed in the Code of Conduct, and
reports about the maintainer go to GitHub's abuse reporting, as described
there.

## 8. Continuity and succession

### 8.1 Bus factor

The bus factor is one. The BDFL is the only maintainer (see
[`MAINTAINERS.md`](MAINTAINERS.md)), the only owner of the GitHub organization
`VMAFx` that holds the repository, and the only holder of the release
credentials listed below. If that person becomes unavailable for an extended
period, review, releases and security advisories stop until access passes on.
This section says what is meant to happen then. Naming a successor is a
decision the BDFL has not yet recorded here; until one is, the steps below
describe the intent and the guidance for the community, and no named person
holds any of the access.

### 8.2 What access exists

- **The GitHub organization and repository** (`VMAFx/vmafx`), including branch
  protection, the required checks, secrets and environments.
- **Release identities.** Release artifacts are signed keyless with Sigstore
  and GitHub OIDC; no long-lived signing key exists, and the signing identity
  is the release workflow inside the repository (see
  [Releases](docs/development/release.md) and
  [ADR-0010](docs/adr/0010-sigstore-keyless-signing.md)). The release
  automation authenticates as a GitHub App, or as a personal access token
  where the App is absent
  ([release-bot identity](docs/development/release.md#release-bot-identity)).
  The protected environments `release-publish` and `pypi-publish` gate
  publication.
- **Container images** under `ghcr.io/vmafx/`, published by the release
  workflows.
- **The documentation site**, published from the repository by GitHub Pages.
- **The project mailbox** named in [`SECURITY.md`](SECURITY.md) and
  [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).
- **Funding accounts.** [GitHub Sponsors](https://github.com/sponsors/lusoris)
  (USD, live since 2026-10-08), [Ko-fi](https://ko-fi.com/lusoris) (EUR) and
  [Patreon](https://www.patreon.com/Lusoris) (EUR) are live. The GitHub profile
  belongs to the maintainer's personal account. Sponsorship buys recognition
  only, listed in [`SPONSORS.md`](SPONSORS.md) and explained on [Support
  VMAFx](docs/support-vmafx.md)
  ([ADR-2689](docs/adr/2689-sponsorship-tiers-recognition-only.md)). Their
  handover is part of the access list above.

### 8.3 Succession plan

1. **Access today.** One person holds every access listed above. A second
   organization owner is not planned at this time; if that changes, the name is
   recorded in [`MAINTAINERS.md`](MAINTAINERS.md).
2. **Handing over.** The successor takes over the organization, the
   repository settings, the release App or token, the protected environments,
   the container registry and the mailbox. Because signing is keyless and tied
   to the repository workflow, no signing key has to be handed over; the
   verification identity stays valid as long as the workflow path in the
   repository stays the same, and changes to it are announced in the release
   notes.
3. **If no successor takes over.** A community member who wants to continue
   the project is welcome to fork it. The licence (EUPL-1.2, and the inherited
   licences of the files that carry them) allows this without permission. A
   fork is a new project: its releases are signed with its own identity and
   its container images live under its own registry, so published
   verification commands name the new identity. The fork should say plainly
   that it is a continuation.
4. **Archiving.** If the project ends, the repository is archived read-only
   on GitHub, not deleted, so that links, release assets and signature bundles
   stay verifiable. The final release notes and the README say that the
   project is no longer maintained and point to the fork, if there is one.

### 8.4 What the community can do now

- Keep contributions in the open (issues, pull requests, ADRs) so the reasoning
  is not held in one person's head.
- Offer to become a maintainer by following
  [Becoming a maintainer](MAINTAINERS.md#becoming-a-maintainer).
- Keep local clones and forks current; release assets carry signature
  bundles and SBOMs (see [Releases](docs/development/release.md)), so a mirror
  can be verified.

## 9. Amending this document

Changes to this `GOVERNANCE.md` follow the normal ADR + PR flow —
the change lands as a PR with a new ADR that cites this file under
`## References`. Substantial governance shifts (e.g., moving from
BDFL to a steering committee) require an ADR.
