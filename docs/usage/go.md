<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->

# Go module and version policy

VMAFx extends Netflix's Video Multi-Method Assessment Fusion (VMAF) video
quality library. Its Go module path is **`github.com/VMAFx/vmafx`**, declared
in the repository-root [`go.mod`](../../go.mod). Package imports append
their repository-relative directory to that path.

## Choose a fork version

Use VMAFx release tags only. Product releases follow the independent SemVer
stream in [ADR-1127](../adr/1127-single-semver-release-stream.md), with the
`0.1.0` restart defined by [ADR-3292](../adr/3292-release-line-restart-0x.md).
Replace `VMAFX_TAG` with an available fork release tag:

```bash
go get github.com/VMAFx/vmafx@VMAFX_TAG
go list -m -json github.com/VMAFx/vmafx
```

Inherited Netflix release tags were deleted under
[ADR-1805](../adr/1805-delete-inherited-netflix-tags.md); those version
numbers do not identify releases of these Go packages. The
[removed-tag record](../development/inherited-tags.md) preserves their
provenance. Upstream syncs must fetch without tags, and the repository's
pre-push guard rejects reintroducing the recorded tags.

Go proxies may retain cached inherited versions after deletion. ADR-1805
records `@latest` selecting Netflix's `v3.0.0+incompatible` from the proxy
after cleanup. Pin an explicit fork tag or commit instead of relying on
`@latest`; this page does not assert the proxy's current response.

## Retractions

The root `go.mod` retains these withdrawn fork versions:

```go
retract (
    v1.0.0-rc.3
    [v1.0.0-rc.1, v1.0.0-rc.2]
)
```

Candidates rc.1 and rc.2 carried stale licence metadata; rc.3 was withdrawn
when the release stream restarted at `0.1.0`. Keep these retractions in
later releases. They warn consumers about withdrawn versions; they do not
delete cached versions or guarantee that a proxy selects a fork release.
See the [Go retraction reference](https://go.dev/ref/mod#go-mod-file-retract).

## Pin a commit

For work before a suitable fork release, replace `COMMIT_SHA` with a full
commit hash from your reviewed VMAFx revision:

```bash
go get github.com/VMAFx/vmafx@COMMIT_SHA
go list -m -json github.com/VMAFx/vmafx
```

Go resolves the revision to a canonical version, usually a pseudo-version
for an untagged commit. Check the resolved `Version`, then retain the
resulting `go.mod` and `go.sum` changes in your consumer repository. Branch
names move; a reviewed commit hash makes the selected revision explicit.
See [Go version queries](https://go.dev/ref/mod#version-queries).
