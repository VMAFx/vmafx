# Design notes: docker-publish-operator-node.yml

These notes were moved out of
`.github/workflows/docker-publish-operator-node.yml` so the workflow stays under
the 64 KiB input bound of the praetor engine. They are the workflow's original
comments, grouped by job, unchanged.

## general

```text
docker-publish-operator-node — build, push, sign, and SBOM the VMAFX Go
service images when a release-please draft is published.

Triggers:
  - publication of a release-please draft (`release.published`)
  - workflow_dispatch at an existing published tag for recovery

Produces:
  ghcr.io/vmafx/vmafx-operator:<tag>   (amd64 + arm64)
  ghcr.io/vmafx/vmafx-operator:latest  (same)
  ghcr.io/vmafx/vmafx-server:<tag>     (amd64 + arm64, Go HTTP/gRPC server)
  ghcr.io/vmafx/vmafx-server:latest    (same)
  ghcr.io/vmafx/vmafx-controller:<tag> (amd64 + arm64, job queue + auth gateway, ADR-1589)
  ghcr.io/vmafx/vmafx-controller:latest (same)
  ghcr.io/vmafx/vmafx-node:<tag>       (amd64 + arm64, CPU variant)
  ghcr.io/vmafx/vmafx-node:latest      (same)

Post-push steps: cosign keyless sign + syft SBOM (CycloneDX JSON) via cosign attest.
Each image also carries its licence texts and passes a licence check in its
build; every platform image gets an SPDX SBOM attested with actions/attest,
and the corresponding source each platform build pushes is merged into
<image>:<tag>-source (.github/actions/image-licence-artifacts, ADR-1513).
GPU node variants (-cuda13, -rocm10) are deferred to a follow-on PR;
-sycl is published for amd64 (ADR-3371).

ADR-0815: operator-node-distroless-dockerfiles
ADR-0698: distroless base-image policy
Workflow-level least-privilege; jobs that push/sign/attest opt in below.
A release tag must fan out only once; never cancel a partially-published set.
Layer compression of every image this workflow pushes (ADR-1594): zstd at
BuildKit's strongest level (22 maps to klauspost's best, about zstd 11).
force-compression converts every layer that is not zstd yet, base-image and
cached layers included; oci-mediatypes because Docker rejects zstd layers under
Docker manifest media types. Pulling needs Docker Engine 23.0 or later (Docker
Desktop 4.19), containerd 1.5 or later, or Podman; docs/usage/docker.md.
```

## validate-release

```text
Release events also fire for tester prereleases (tester-*, tester-windows-*);
only version tags (v*) belong to this workflow. Dependent jobs skip with it.
ADR-1201: release candidates publish too; the tag and the release
flag must agree rather than prereleases being refused outright.
The release's prerelease flag is read from the API below: a
workflow_dispatch event carries no release payload.
ADR-1151 — the `latest` moving tag used to be gated on
`github.event_name == 'release'`, so the documented recovery
dispatch (docs/development/release.md) republished the versioned
tag but left `latest` pointing at whatever digest the broken
original run had pushed. Resolve it from the repository's actual
newest published release instead, which makes the guard identical
on both trigger paths.
A repository without a non-prerelease release answers 404: no
`latest` yet. Any other failure stops the publish.
```

## build-operator

```text
ADR-1349: each architecture builds on its own native runner, as
vmafx-node does; under QEMU the arm64 Go build took 30-35 minutes.
publish-operator merges the two digests into the published index.
ADR-1347: a recovery dispatch builds the tag's source with the build
recipe of the default-branch commit it runs from (ADR-1350: including
the patches to the bundled FFmpeg); nothing else moves.
ADR-1503 rule 5 / ADR-1513: the corresponding source of this platform
image, built from the same stages and pushed by digest; the publish job
merges both platforms into <tag>-source.
```

## publish-operator

```text
ADR-1503 / ADR-1513: SPDX SBOM per platform attested on its digest, and
the platform source images merged into <tag>-source.
```

## build-server

```text
ADR-1349: each architecture builds on its own native runner, as
vmafx-node does; under QEMU the arm64 libvmaf + CGO build took 46 minutes of its 60.
publish-server merges the two digests into the published index.
ADR-1347: a recovery dispatch builds the tag's source with the build
recipe of the default-branch commit it runs from (ADR-1350: including
the patches to the bundled FFmpeg); nothing else moves.
ADR-1503 rule 5 / ADR-1513: the corresponding source of this platform
image, built from the same stages and pushed by digest; the publish job
merges both platforms into <tag>-source.
```

## publish-server

```text
ADR-1503 / ADR-1513: SPDX SBOM per platform attested on its digest, and
the platform source images merged into <tag>-source.
```

## build-controller

```text
ADR-1589: the controller image, built like vmafx-server (its Dockerfile
mirrors Dockerfile.go-server's licence-gated stages); ADR-1349: each
architecture on its own native runner. publish-controller merges the two
digests into the published index.
ADR-1347: a recovery dispatch builds the tag's source with the build
recipe of the default-branch commit it runs from (ADR-1350: including
the patches to the bundled FFmpeg); nothing else moves.
ADR-1503 rule 5 / ADR-1513: the corresponding source of this platform
image, built from the same stages and pushed by digest; the publish job
merges both platforms into <tag>-source.
```

## publish-controller

```text
ADR-1503 / ADR-1513: SPDX SBOM per platform attested on its digest, and
the platform source images merged into <tag>-source.
```

## build-node

```text
ADR-1349: each architecture builds on its own native runner. Under QEMU
the arm64 FFmpeg + libvmaf + CGO build ran past the 120-minute limit on
every v1.0.0-rc.1 attempt, and a timed-out job never exports its cache.
publish-node merges the two digests into the published multi-arch index.
ADR-1347: a recovery dispatch builds the tag's source with the build
recipe of the default-branch commit it runs from (ADR-1350: including
the patches to the bundled FFmpeg); nothing else moves.
ADR-1503 rule 5 / ADR-1513: the corresponding source of this platform
image, built from the same stages and pushed by digest; the publish job
merges both platforms into <tag>-source.
```

## publish-node

```text
ADR-1503 / ADR-1513: SPDX SBOM per platform attested on its digest, and
the platform source images merged into <tag>-source.
```

## publish-node-sycl

```text
ADR-1503 / ADR-1513 / ADR-3371: SPDX SBOM attested on the digest, and the
corresponding source of the copyleft parts as <tag>-sycl-source.
```

## smoke-test

```text
ADR-1589: entrypoint, nonroot user, --version, and an HTTP start with
auth disabled whose SQLite queue lands in the image's /data.
Remote storage inputs run rclone (pkg/storage, ADR-0719); mount mode
needs the FUSE tools (ADR-1593).
```
