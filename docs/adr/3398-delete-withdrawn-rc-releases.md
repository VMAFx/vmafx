<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3398: Delete the withdrawn 1.0.0 candidates and tester releases after 0.1.0

- **Status**: Accepted
- **Date**: 2026-11-03
- **Deciders**: lusoris
- **Tags**: `release`, `governance`, `supply-chain`, `docker`, `github`

## Context

Under [ADR-3292](3292-release-line-restart-0x.md), VMAFx restarted its release line at `v0.1.0`, designating `1.0.0` as the first stable release once the Rust core (Phase P3) lands and the public interface is frozen. Decision 3 of ADR-3292 specified that candidates `v1.0.0-rc.1`, `v1.0.0-rc.2`, and `v1.0.0-rc.3` would remain on the forge with their tags, images, and assets marked as withdrawn, and that nothing would be deleted.

In practice, retaining the withdrawn candidates and ad-hoc hardware tester releases on GitHub and GitHub Container Registry (GHCR) causes ongoing maintainer and user confusion. Downstream consumers and automated discovery can still pull defunct container images or release archives that predate critical licence corrections ([ADR-1513](1513-production-artifact-licensing.md), [ADR-1699](1699-root-licence-files-eupl.md)) or contain superseded runtime behaviours. Furthermore, the presence of nine withdrawn releases and tags (`v1.0.0-rc.1` through `rc.3` plus six `tester-*` pre-releases) clutters release listings and forge views.

The maintainer decided to clean up the forge after `v0.1.0` is published and signed (References). Because `release-please` derives its state from the repository manifest and the latest forge release, deleting the candidate releases while the manifest still references `1.0.0-rc.3` would break release automation. Ordering is therefore strictly sequenced: `v0.1.0` is cut, verified, and signed first; only then are the obsolete candidate and tester releases and tags removed.

## Decision

1. **Deletion timing and release-please sequencing.** Deletion occurs strictly after `v0.1.0` has been published and its cryptographic signatures verified. `release-please` reads `release-please-config.json`, `.release-please-manifest.json`, and the latest GitHub release. Deleting the `v1.0.0-rc.*` releases prior to the `v0.1.0` cut would remove the release corresponding to the current manifest baseline (`1.0.0-rc.3`). Once `v0.1.0` lands on `master`, the manifest advances to `0.1.0`, decoupling automation from the withdrawn candidates.
2. **GitHub releases and tags to delete.** The following GitHub releases and their associated Git tags will be deleted from `VMAFx/vmafx`:
   - `v1.0.0-rc.1`
   - `v1.0.0-rc.2`
   - `v1.0.0-rc.3`
   - `tester-20261004-4d3792b3`
   - `tester-20261004-860050c3`
   - `tester-20261005-9bc68a10`
   - `tester-20261007-40e87b15`
   - `tester-windows-20261004-2889f963`
   - `tester-windows-20261007-40e87b15`
3. **GHCR image versions to delete.** All package versions tagged with `v1.0.0-rc.*` (including variant tags such as `*-tester*`, `*-source`, `-server`, `-cuda13`, `-rocm10`, and `-sycl`) will be deleted across the following GitHub Container Registry packages:
   - `ghcr.io/vmafx/vmafx`
   - `ghcr.io/vmafx/vmafx-server`
   - `ghcr.io/vmafx/vmafx-operator`
   - `ghcr.io/vmafx/vmafx-controller`
   - `ghcr.io/vmafx/vmafx-node`
   Packages under `pelorus` and `pelorus-dev` are independent and outside the scope of this cleanup.
4. **Retained assets and tags.** The following tags and assets are explicitly preserved:
   - Model and fixture assets: `fixtures-hdr-ictcp-pixels-v1`, `fixtures-pq-sollevante-v1`, `tiny-blobs-v1`.
   - Licensing archival tag: `archive/eupl-relicense-v1` ([ADR-1250](1250-eupl-fork-relicense.md)).
   - Historical test and run logs: preserved in git history and changelog archives ([`docs/changelog-archive/`](../changelog-archive/)).
5. **Go module retractions preserved.** The retraction block in the root `go.mod` retracting `v1.0.0-rc.1`, `v1.0.0-rc.2`, and `v1.0.0-rc.3` remains permanently. Although the public Go module proxy (`proxy.golang.org`) caches downloaded modules immutably, the retraction directives inform direct-mode consumers (`GOPROXY=direct`) that these versions are invalid.
6. **Future 1.0.0 candidate numbering.** Release candidates for `1.0.0` will start at `v1.0.0-rc.4`, maintaining Decision 1 of ADR-3292. Because `proxy.golang.org` permanently retains `rc.1` through `rc.3`, reissuing those version numbers would cause checksum mismatches and serve stale cached content to Go consumers.
7. **PyPI candidate status.** The yanking of `vmaf-mcp` versions `1.0.0rc1`, `1.0.0rc2`, and `1.0.0rc3` from PyPI remains a manual maintainer action following the publication of `v0.1.0` on PyPI (Decision 5 of ADR-3292 unchanged).
8. **Reviewed inventory.** Before any deletion command runs, the complete inventory of target releases, tags and container versions is reviewed and approved by the maintainer. Deletion runs only against that explicit inventory, never against a pattern.

This ADR supersedes Decision 3 of [ADR-3292](3292-release-line-restart-0x.md). All other provisions of ADR-3292 (version line restart at 0.1.0, candidate restart at rc.4, Go retractions, PyPI yanking) remain in effect.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Retain candidate releases and tags marked withdrawn (ADR-3292 D3) | Zero post-release deletion actions; existing URLs remain valid | Confuses users; pollutes GitHub release list and GHCR package feeds; leaves defective early images in circulation | Superseded by maintainer decision once 0.1.0 is published |
| Delete candidates immediately before cutting 0.1.0 | Cleans the forge before new release lands | Breaks `release-please` automation, which relies on the manifest-matching GitHub release (`1.0.0-rc.3`) to determine history | Rejected due to hard release-please dependency ordering |
| Reuse `v1.0.0-rc.1`..`rc.3` for future 1.0.0 candidates | Sequential numbering starting from rc.1 | `proxy.golang.org` immutably caches early candidates; reusing tags results in checksum mismatches and corrupted dependencies | Rejected; ADR-3292 D1 start at rc.4 is preserved |
| Delete inherited Netflix tags or archive tags | Slightly cleaner tag namespace | Breaks upstream history provenance; violates ADR-1250 and ADR-1805 | Tags are retained |

## Consequences

- **Positive**: GitHub Releases page and GHCR registry represent only active and supported versions; obsolete and potentially misleading container images are removed from distribution channels; `release-please` continues functioning normally without disruption.
- **Negative**: Deep links to withdrawn `v1.0.0-rc.*` and `tester-*` release downloads or image digests will return 404; users must reference `v0.1.0` or git commits directly.
- **Neutral / Follow-ups**:
  - The maintainers review the inventory and run the deletions once `v0.1.0` is published and signed.
  - The maintainer yanks `vmaf-mcp` 1.0.0rc1–rc3 on PyPI after `0.1.0` is published.
  - Documentation references to early candidates are updated to reflect their historical and deleted status.
  - The root `go.mod` retractions and the `release-please-config.json` comments are preserved.

## References

- Maintainer decision: ledger id `vmafx-old-rc-delete-after-010-20261103` (paraphrased): delete GitHub releases, tags, and GHCR images for `v1.0.0-rc.1`–`rc.3` and the six tester releases after `v0.1.0` is published and signed; preserve fixtures, model blobs, and EUPL relicense tags; keep `go.mod` retractions; restart candidates at `rc.4`.
- Maintainer decision: ledger id `vmafx-milestone-010-full-speed-20261103` (paraphrased): streamline milestone 0.1.0 delivery and cleanup procedures.
- Prior decisions: [ADR-3292](3292-release-line-restart-0x.md) (release line restart at 0.1.0), [ADR-1151](1151-vmafx-first-release-1-0-0.md) (first release cut-over), [ADR-1201](1201-release-candidates-before-1-0-0.md) (release candidate governance), [ADR-1348](1348-release-candidate-prerelease-versioning.md) (prerelease versioning strategy), [ADR-1805](1805-delete-inherited-netflix-tags.md) (deletion of inherited upstream tags).
