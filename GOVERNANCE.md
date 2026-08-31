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
need to sign a CLA — by submitting code, they agree to license under
BSD-3-Clause-Plus-Patent (see [`LICENSE`](LICENSE) and
[`CONTRIBUTING.md`](CONTRIBUTING.md)).

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
- SLSA L3 provenance (`slsa-verifier ...`).

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
maintainers; reports go to the address listed in the Code of Conduct.

## 8. Amending this document

Changes to this `GOVERNANCE.md` follow the normal ADR + PR flow —
the change lands as a PR with a new ADR that cites this file under
`## References`. Substantial governance shifts (e.g., moving from
BDFL to a steering committee) require an ADR.
