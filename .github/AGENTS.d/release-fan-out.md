---
paths:
  - .github/workflows/release-please.yml
  - .github/workflows/supply-chain.yml
  - scripts/release/verify-release-version.sh
  - dev/Containerfile
invariant: Single root package draft release; release-publish environment; native release-build stage; tag-bound recovery.
---
# Single SemVer release fan-out and supply chain (ADR-1127)

## Single SemVer release fan-out (ADR-1127)

`release-please.yml` owns one root package, creates draft GitHub release.
Publishing that draft is authenticated operation that creates `vX.Y.Z`
tag (`vX.Y.Z-rc.N` for ADR-1201 release candidate), starts
`supply-chain.yml` plus both Docker publication workflows. Never
split `release-please` back into unqualified component tags or make release
non-draft without first providing and validating explicit downstream
workflow trigger. Workflow pauses both `release-please` phases while one
`vX.Y.Z` or `vX.Y.Z-rc.N` draft exists; later master pushes must not move or
duplicate release waiting at human publication gate. Draft step's inline jq
tag regex = exact shape `scripts/release/verify-release-version.sh` accepts;
narrower hides waiting draft, re-runs `release-please` against untagged
release. `scripts/release/tests/test-release-please-draft-gate.sh` extracts
step from workflow, proves parity. First cut (1.0.0-rc.1, ADR-1151 /
ADR-1201) and 0.1.0 restart (ADR-3292) each selected by one-time
`release-as` config field; release-PR rollover removes that field and
`bootstrap-sha` before release PR merges, so neither override can affect
later releases; `scripts/release/check-one-shot-fields.sh` refuses survivors.
Later forced versions: `Release-As:` footer.

Every job publishing to GHCR, uploading GitHub Release assets, or minting
release-artifact Sigstore identity is bound to protected
`release-publish` environment. PyPI stays bound to `pypi-publish` because that
exact environment name is part of its Trusted Publisher identity. Both
environments accept ordinary release tags only, require configured
release reviewer; read-only validation stays outside them. Provenance jobs
`provenance` / `mcp-provenance` (ADR-1356) hold `id-token` + `attestations`
write, `contents: read`, so they sit in `release-publish` too; they upload
bundle only as workflow artifact. Environment-gated attachment job is sole
release-asset writer. Never give provenance jobs `contents: write` or
release-upload step.

`supply-chain.yml` runs `scripts/release/verify-release-version.sh` before any
write or OIDC job. It keeps native and `vmaf-mcp` hashes in distinct provenance
jobs with distinct bundle names (`vmafx-build-provenance.sigstore.json`,
`vmaf-mcp-provenance.sigstore.json`). Subjects = build job `hashes` output,
diffed against downloaded bytes; each job runs consumer
`gh attestation verify --bundle` recipe on every subject before upload.
Never restore `slsa-framework/slsa-github-generator`: its reusable workflow
calls own sub-actions by tag, org `sha_pinning_required` rejects it
(v1.0.0-rc.2 `detect-env` failure). Native SBOMs contain and hash every
staged artifact; Python SBOMs inventory installed `vmaf-mcp` dependency
graph plus wheel and sdist. Workflow fails if either inventory becomes
empty or mislabeled. Anchore's implicit artifact/release uploads stay disabled:
explicit SBOM artifact feeds keyless signing before final strict
attachment job. Never bypass that DAG or restore permissive unmatched-file
uploads; green workflow must mean every promised asset exists.

Native payload built on `ubuntu-26.04` inside `release-build` stage of
tagged `dev/Containerfile` (ADR-1346, supersedes ADR-1178; ADR-1354 moved it
to Debian 13 release track): stage built in job by
`scripts/ci/build-dev-container-stage.sh release-build` (no GITHUB_TOKEN:
stage mounts no secret; no external layer cache, no registry), compile by
`scripts/release/build-native-release-artifacts.sh` under
`docker run --pull never --network none` as runner UID. No job-level
concurrency group on `build-artifacts` (GitHub cancels older pending job in
group); workflow-level per-tag group stays. Dev Container PR gate
rehearses both steps (same script, same `docker run`, local tag
`v<manifest version>` on HEAD so describe matches tag build); change one,
change other. Never restore self-hosted `sycl-arc` label, host compile, or
GHCR pull for release build, or `build-deps` (Ubuntu 26.04, glibc 2.43)
as release stage. `verify-native-artifacts` pinned `ubuntu-24.04`, oldest
hosted image that loads bundle (floor GLIBC_2.38 + GLIBCXX_3.4.30;
`ubuntu-22.04` fails); never `ubuntu-latest`. Same job starts downloaded CLI
on `RELEASE_RUNTIME_CC` (distroless `cc-debian13`, from `build-config.env`)
with no `LD_LIBRARY_PATH`: proves RUNPATH `$ORIGIN` there; never add it back.

Native payload is Linux ELF, materializes complete Meson
`libvmaf.so` / SONAME / real-name chain as regular files. Before any native
write or OIDC job, artifact round-trip verifier must prove
downloaded `vmaf` resolves its declared SONAME from that directory, reports
release version under `env -i`. Hashing, SBOM generation, signing, build
provenance, and strict attachment cover every materialized chain name.

Manual supply-chain recovery must use published tag as both workflow ref
and input (`gh workflow run supply-chain.yml --ref "$tag" -f tag="$tag"`).
Validation job rejects branch-ref dispatches, missing/draft/prerelease releases,
or event SHA different from checked-out tag. This binding is required so
build provenance describes source that produced artifacts. Container
signature verification requires exact `@refs/tags/${PUBLISH_TAG}` workflow
identity; wildcard ref would accept signatures minted by branch workflows.
