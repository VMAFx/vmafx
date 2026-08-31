---
name: prep-release
description: Dry-run release-please locally, preview the CHANGELOG diff, verify signing prerequisites (Sigstore/OIDC), and surface any blockers before a release PR merges.
---

<!-- markdownlint-disable MD013 -->

# /prep-release

## Invocation

```text
/prep-release [--next-version=auto|<semver>]
```

## Steps

1. Check prerequisites:
   - `gh auth status` — logged in, has `workflow` scope.
   - `cosign version` — ≥ 2.2 (keyless requires modern cosign).
   - Every signing/publishing job that needs OIDC has job-scoped
     `id-token: write` in `supply-chain.yml` and the two Docker publication
     workflows.
2. Run `release-please release-pr --dry-run --repo-url=<fork>` (via the
   release-please CLI or a scripted equivalent) from an origin-faithful clone.
   This repository fetches Netflix upstream tags into the shared local tag
   namespace; a dry-run against the ordinary development checkout can mistake
   an upstream-only `vX.Y.Z` tag for a VMAFx release and bypass the configured
   bootstrap SHA. The simulation clone must contain only tags advertised by
   `VMAFx/vmafx`, plus the candidate master tree. Pass the GitHub token through
   a protected file descriptor or file path, never as a literal command-line
   value.
3. Parse the proposed release: version bump, CHANGELOG delta, affected packages.
4. Display the diff: version old → new, CHANGELOG section added, tag that will be
   created.
5. Verify the version matches the ordinary SemVer scheme
   `vMAJOR.MINOR.PATCH` (ADR-1127).
6. Report supply-chain prerequisites:
   - SBOM generator present (`syft`, `cyclonedx-cli`).
   - SLSA generator workflow configured.
   - Container image build target present (if applicable).
7. Summary: GO / NO-GO + blocker list.

## Notes

- This skill never creates releases. It only previews what the next release-please PR
  would propose, so the operator can merge it with confidence.
- If the proposed version is not ordinary SemVer or the root-owned package
  versions diverge, surface it as a blocker and repair
  `release-please-config.json` before merging.
