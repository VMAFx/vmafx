---
paths:
  - scripts/release/pep440-version.sh
  - scripts/release/tests/test-pep440-version.sh
  - .github/workflows/supply-chain.yml
invariant: Exact X.Y.Z / X.Y.Z-rc.N -> PEP 440 filenames without packaging; SBOM keeps SemVer.
---
<!-- markdownlint-disable MD013 MD060 -->
# PEP 440 wheel/sdist version converter (`pep440-version.sh`, ADR-1201)

## pep440-version.sh (ADR-1201)

Tag, manifest, markers carry SemVer spelling (`1.0.0-rc.1`). Hatchling
names vmaf-mcp wheel + sdist after PEP 440 normalized version:
`vmaf_mcp-1.0.0rc1-py3-none-any.whl`, `vmaf_mcp-1.0.0rc1.tar.gz`.
`supply-chain.yml` `validate-release` derives `pep440_version` output
once via this script; every version-bound `vmaf_mcp-*` filename glob and
PyPI JSON release URL use it (`VMAFX_PEP440_VERSION`). SemVer glob
matches zero wheels on RC -> `mcp-build` fails, SBOM / signing / PyPI /
attachment skipped.

Converter exact for verifier's two shapes only: `X.Y.Z` -> `X.Y.Z`,
`X.Y.Z-rc.N` -> `X.Y.ZrcN`; anything else exit 64, no output. No
`packaging` import: `validate-release` installs nothing, and ADR-1305
forbids unhashed install. Widen verifier shape -> widen converter + test
in same PR.

SBOM identity checks stay on SemVer `version`, not `pep440_version`:
hatchling 1.32 writes pyproject spelling (`1.0.0-rc.1`) into METADATA;
Syft 1.51 copies it verbatim into SPDX `versionInfo` and purl
`pkg:pypi/vmaf-mcp@1.0.0-rc.1`. Filenames normalized, metadata not --
never "fix" one to match other. Hatchling or Syft bump -> re-check both.

Version-bound sdist pattern has no wildcard: nullglob keeps literal
path. Every version-bound count check also requires
`-f "${sdists[0]}"`.

Test coverage: `scripts/release/tests/test-pep440-version.sh`
(converter cases, workflow wiring, glob resolution against
hatchling-shaped filenames with decoys, SemVer-regression fixtures).
