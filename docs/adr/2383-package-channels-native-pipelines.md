<!-- markdownlint-disable MD013 MD060 -->
# ADR-2383: macOS and Windows package channels are fed by the verified native release pipelines

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer
- **Tags**: release, packaging, supply-chain, windows, macos, fork-local

## Context

RC4 work package 12 (#2437) ships the package channels of the distribution manifest
(#2314): a Homebrew tap, conda-forge, winget, signed apt and rpm repositories, the Python
wheel (#2318) and the language registries (#2320). Homebrew and winget packages
install macOS and Windows binaries. [ADR-1102](1102-phase4b9-container-only-publishing.md)
requires every published artifact to be produced inside the container, and there is no
macOS or Windows build path in the container. [ADR-1493](1493-macos-tester-bundle.md)
and [ADR-1515](1515-windows-tester-zip.md) already carry bounded exceptions for the
tester bundles built on hosted runners; the channels need the same footing for release
packages, or the macOS and Windows entries of the manifest cannot exist.

## Decision

The macOS and Windows artifacts that the package channels distribute come from the
repository's native release pipelines, under the bounds of ADR-1515:

- built only by the named workflows on the hosted macOS and Windows runners, from a commit
  reachable from `master` or from a published release tag, with the recipe of the
  dispatching ref;
- each artifact carries a build-provenance attestation, an SBOM attestation and a cosign
  keyless bundle, and is published by the release-bot identity behind the protected
  publishing environment;
- the distribution manifest (#2314) names the workflow run and digest of every such
  artifact, and the drift check fails when a channel points at an artifact that is not in
  the manifest or whose attestation does not verify;
- the channel metadata (formula, manifest, recipe) is generated from the manifest and
  contains the digest; a package built or uploaded by hand is refused;
- Linux artifacts, container images and the Linux apt and rpm packages stay container-built
  under ADR-1102; this decision does not widen the exception to them.

The exception ends when a macOS or Windows build path exists in the container, and is
recorded in the project exception list with that trigger.

## Alternatives considered

| Option | Pros | Cons | Outcome |
|---|---|---|---|
| Native pipelines with attestation, bounded as ADR-1515 (**chosen**) | Reuses verified, attested pipelines; channels exist at 1.0 | A named exception to ADR-1102 | Chosen |
| Container-only, no macOS or Windows channels | No exception | No Homebrew or winget package; the manifest has holes on two platforms | Not chosen |
| Cross-compile macOS and Windows in the container | Keeps ADR-1102 whole | No usable Apple SDK licence path; MSVC cannot be redistributed in a container | Not chosen |
| Build channel packages by hand on a maintainer machine | Fast | Unattested, not reproducible | Not chosen |

## Consequences

- **Positive**: the manifest covers Linux, macOS and Windows with one verification rule.
- **Negative**: one more entry in the exception list to retire later.
- **Neutral / follow-ups**: #2314 states the rule in the manifest schema; #2319 and #2437
  cite this ADR.

## References

- Maintainer decision of 2026-10-07 (ledger Q-161 to Q-164): macOS and Windows package
  channels come from their verified, attested native pipelines under a bounded exception
  ADR.
- [ADR-1102](1102-phase4b9-container-only-publishing.md), [ADR-1493](1493-macos-tester-bundle.md),
  [ADR-1515](1515-windows-tester-zip.md)
- Issues [#2437](https://github.com/VMAFx/vmafx/issues/2437),
  [#2314](https://github.com/VMAFx/vmafx/issues/2314),
  [#2319](https://github.com/VMAFx/vmafx/issues/2319)
