- The release pipeline can now cut `v1.0.0-rc.N` release candidates before the final
  `v1.0.0`. Six separate guards previously refused or mishandled a prerelease — the version
  verifier's tag regex and its release-marker extractor, two checks in `supply-chain.yml`,
  and one each in the production and operator-node image workflows. Each blanket rejection
  is replaced by a **consistency** check: an `-rc.N` tag must be published as a prerelease
  and a final tag must not, because the mismatched states are the dangerous ones. A release
  candidate can never take the `latest` image tag. The accepted suffix is deliberately
  narrow — `rc` only, dotted integer, no leading zero. See
  [ADR-1201](docs/adr/1201-release-candidates-before-1-0-0.md).
