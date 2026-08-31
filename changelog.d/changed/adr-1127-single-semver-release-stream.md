- Replace the fork-suffixed component release layout with one independent VMAFx
  SemVer stream, beginning with the `3.2.1` patch release.
- Update governance, security, C API, and MCP examples to the ordinary SemVer
  contract; document the transferred repository's exact PyPI Trusted Publisher
  identity and make release signing/hash loops safe for dash-prefixed assets.
- Coordinate the compatibility `vmaf` Python package with the same version and
  make publication fail closed on tag/version drift, missing assets, unsigned
  or empty SBOMs, provenance source/collision errors, or immutable PyPI
  filename/hash divergence before and after trusted publication.
- Keep the 3.2.1 override strictly one-time, pause automation behind any
  unpublished release draft, and retire both cutover fields during release-PR
  changelog rollover.
- Make release dry-runs ignore upstream-only local tags so release-please tests
  the fork's real bootstrap and release history.
