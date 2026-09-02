- Renovate: two rules that stop the dependency train proposing PRs that can
  never pass. (1) `slsa-framework/slsa-github-generator` is never
  digest-pinned (`pinDigests: false`; tag bumps still flow) — the generator's
  README requires an exact `@vX.Y.Z` reference so `slsa-verifier` can verify
  the trusted builder, and the Release Script Contract check enforces that
  form, so the digest-pin PR #1183 could not merge. (2) `intel/compute-runtime`,
  `intel/intel-graphics-compiler`, `intel/gmmlib` and `oneapi-src/level-zero`
  are grouped into one PR: `dev/Containerfile` downloads
  `libigdgmm12_${GMMLIB_VER}` and the IGC debs from the compute-runtime release
  page for `NEO_VER`, so a lone gmmlib bump produced a 404 and a red
  `Dev Container Build` (#1184). Both stay manual-review per ADR-0605.
