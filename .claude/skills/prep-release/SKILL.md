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
   - `gh auth status`: logged in, has `workflow` scope.
   - `cosign version`: ≥ 2.2 (keyless requires modern cosign).
   - Every signing/publishing job needing OIDC: job-scoped `id-token: write`
     in `supply-chain.yml` and 2 Docker publication workflows.
   - `bash scripts/release/check-release-bot-secrets.sh`: 2 release-bot secret
     names exist (ADR-1151 / ADR-1171). NO-GO if exit non-zero; master workflow
     stays idle-green without them, will not say so.
2. Run `release-please release-pr --dry-run --repo-url=<fork>` (via
   release-please CLI or scripted equivalent) from origin-faithful clone.
   Repo fetches Netflix upstream tags into shared local tag namespace;
   dry-run against ordinary development checkout can mistake upstream-only
   `vX.Y.Z` tag for VMAFx release -> bypasses configured bootstrap SHA.
   Simulation clone must contain only tags advertised by `VMAFx/vmafx`, plus
   candidate master tree. Pass GitHub token via protected file descriptor or
   file path, never as literal command-line value.
3. Parse proposed release: version bump, CHANGELOG delta, affected packages.
4. Display diff: version old -> new, CHANGELOG section added, tag created.
5. Verify version matches ordinary SemVer scheme `vMAJOR.MINOR.PATCH`
   (ADR-1127).
6. Report supply-chain prerequisites:
   - SBOM generator present (`syft`, `cyclonedx-cli`).
   - SLSA generator workflow configured.
   - Container image build target present (if applicable).
7. Summary: GO / NO-GO + blocker list.

## Notes

- Skill never creates releases. Only previews what next release-please PR
  would propose, so operator merges with confidence.
- If proposed version not ordinary SemVer or root-owned package versions
  diverge: surface as blocker, repair `release-please-config.json` before
  merging.
