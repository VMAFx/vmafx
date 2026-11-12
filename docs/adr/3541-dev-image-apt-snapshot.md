# ADR-3541: Pin Ubuntu development packages to one archive snapshot

- **Status**: Accepted
- **Date**: 2026-11-12
- **Tags**: docker, dev, supply-chain, renovate

## Context

The three `docker/dev/ubuntu-26.04*.Dockerfile` images pin their base image
digests but resolve Ubuntu packages against the live archive. The same source
revision can therefore install different package versions on different days.
`scripts/setup/ubuntu.sh` also refreshes indexes and installs packages, so
passing a snapshot argument only to the Dockerfile's first refresh is insufficient.

The snapshot service redirects HTTP requests to HTTPS. The minimal Ubuntu base
lacks the certificate bundle needed to authenticate that connection. Snapshot
support is present in the measured Ubuntu 24.04 APT 2.8.3, including the
persistent `APT::Snapshot` setting. The pinned oneAPI image was unavailable
locally; the 24.04 measurement used
`ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852`
instead. This establishes the APT series behavior, not vendor-image acceptance.
The pinned Ubuntu 26.04 base reported APT 3.2.0. On 2026-11-12 the snapshot
service first answered HTTP `503` for every suite, and both snapshot refresh
forms exited `100`: an outage fails the build instead of falling back. A later
probe the same day answered `200` for the pinned timestamp on `resolute`,
`resolute-updates` and `resolute-security`. Replaying the image sequence on the
pinned base then resolved every candidate from
`https://snapshot.ubuntu.com/ubuntu/20261101T000000Z` (`apt-cache policy`), and
`apt-get install --print-uris curl git sudo build-essential cmake` named only
that host for all 94 downloads. The same pinned refresh still fetched the live
`archive.ubuntu.com` and `security.ubuntu.com` indexes, because APT keeps each
original source as a shadowed entry next to its snapshot; `apt-cache policy`
does not list those entries.

## Decision

`build-config.env` owns `UBUNTU_APT_SNAPSHOT`, initially `20261101T000000Z`.
All three images copy and source that file, then write
`/etc/apt/apt.conf.d/50ubuntu-snapshot`. Its `APT::Snapshot` setting applies to
every subsequent package refresh and installation, including those made by
the setup script and CUDA installer. The next clause checks the written file
against the `YYYYMMDDTHHMMSSZ` format, so an unset or malformed value fails the
build before any package operation reads it. The existing fail-closed refresh
flags `--error-on=any -o Acquire::Retries=3` remain required.

The named `UBUNTU_APT_CERTIFICATE_BOOTSTRAP` is the only installation from the
live archive: refresh indexes and run no package command except installing
`ca-certificates` and its required dependencies before enabling the snapshot.
Discard these live indexes before the pinned refresh. TLS verification and
Ubuntu archive signature checks remain enabled. Hadolint's DL3008 asks for
`package=version` arguments; each image's RUN carries a scoped
`# hadolint ignore=DL3008` because its versions come from the snapshot.
This exception means the complete image is not bit-for-bit reproducible.

Renovate owns timestamp proposals through a regex custom manager and a custom
datasource. The datasource reads the Ubuntu snapshot service homepage and uses
Renovate's documented JSONata transformation support to calculate UTC midnight
seven days before the run. The service accepts timestamps rather than exposing
a release listing; the generated date is a candidate that must pass an image
build before merge. A package rule permits proposals before 06:00 on Mondays,
in the configured `Europe/Vienna` timezone, with automatic merging disabled.
No separate bot workflow is introduced.

## Consequences

- Ubuntu package resolution stays fixed until the shared timestamp changes.
  Security fixes require advancing that pin and rebuilding the images.
- The bootstrap's certificate package and dependencies still move with the
  live archive; this is a named HISS-11 gap rather than a full hermeticity claim.
- Pinned refreshes also download the shadowed live archive indexes. They do not
  resolve packages, but a build needs both `archive.ubuntu.com` and
  `snapshot.ubuntu.com` reachable. A snapshot service outage fails the refresh
  with exit code `100` rather than falling back to the live archive.
- NVIDIA packages retain the existing CUDA release lock. Intel repository
  pinning remains separate work, tracked by issue 3190. A global `APT::Snapshot`
  is optional per source: APT applies it where a snapshot server is known from
  the Release file or `Acquire::Snapshots::URI` configuration, and leaves other
  repositories, such as NVIDIA's, on their own URL.
- `UbuntuAptSnapshotContract` in `scripts/ci/test_fail_closed_ci.py` checks the
  copy, source, persistent setting, timestamp check, installer ordering and
  exact bootstrap boundary. Planted unpinned installs, any other package
  command before the pin and snapshot overrides must fail it.
- Maintainers may advance the timestamp manually using the same review and
  build process described in [the image guide](../development/base-images.md#ubuntu-development-package-snapshot).

## References

- [Ubuntu Snapshot Service](https://snapshot.ubuntu.com/): persistent
  `APT::Snapshot` configuration and timestamp format.
- [Renovate custom datasource](https://docs.renovatebot.com/modules/datasource/custom/):
  `format` and `transformTemplates`; documentation checked 2026-11-12.
- [Renovate regex manager](https://docs.renovatebot.com/modules/manager/regex/).
- [Renovate regex versioning](https://docs.renovatebot.com/modules/versioning/regex/).
- [Renovate schedule](https://docs.renovatebot.com/configuration-options/#schedule).
- Renovate `44.234.0`, image
  `sha256:247f6652229c20ae77f868905b09f9acd658ede7c498dd94f69ed7268c33b676`:
  bundled JSONata `2.2.1` evaluated the timestamp transformation successfully.
- [ADR-3537: Fail-closed APT refreshes](3537-apt-update-fail-closed.md).
