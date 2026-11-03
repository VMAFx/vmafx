<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3399: Rust crates published from the release pipeline

- **Status**: Accepted
- **Date**: 2026-11-03
- **Deciders**: lusoris
- **Tags**: `rust`, `release`, `ci`, `supply-chain`
- **Supersedes**: The Rust crates' “not published from the release pipeline” clause in [ADR-1127](1127-single-semver-release-stream.md) and [ADR-1151](1151-vmafx-first-release-1-0-0.md); independent crate versioning remains in force.

## Context

VMAFx extends Netflix's Video Multi-Method Assessment Fusion (VMAF) video quality library. Its Rust crates `vmafx-sys` (raw FFI) and `vmafx` (safe wrappers) are independently versioned; the wrapper declares both a local path and a registry version for its `vmafx-sys` dependency. The packaged-crate check in `scripts/ci/check_rust_crate_package.py` verifies both archives and their standalone documentation, but does not publish them.

crates.io trusted publishing requires a crate to have been published already. The initial publication therefore needs an API token; later releases can use GitHub Actions OIDC. Cargo 1.99 supports publishing both packages in one invocation, verifies unpublished workspace dependencies together, and waits for registry indexing between uploads.

## Decision

Publish `vmafx-sys` before `vmafx` from the `crates-publish` job in `.github/workflows/supply-chain.yml`, at a validated final `v*` release tag only; a prerelease tag (a version containing `-`) never publishes, so an rc cannot ship the crates' versions early. The condition reads the version from `validate-release`, so the `workflow_dispatch` recovery path at the same tag publishes too. The job requires the canonical GitHub repository, tag-bound release validation, native-artifact verification, signing and provenance. It uses the existing protected `release-publish` environment with `contents: read` and `id-token: write`. There is no separate manual crate-publication path; recovery uses the existing release workflow at the same published tag.

Read each crate version from its manifest and query its crates.io version endpoint with a repository-identifying User-Agent and a bounded timeout. HTTP 200 skips that version; HTTP 404 selects it for publication. Transport failures and every other status fail the job. Packaging is verified in an uncredentialed `crates-verify` job (`contents: read` only) via a locked dry-run build; `crates-publish` then uploads selected packages with `cargo publish --locked --no-verify --registry crates-io -j4`, ordering `vmafx-sys` before `vmafx`. This ensures build scripts never execute with publish credentials or OIDC tokens present. Cargo handles dependency order and index visibility; failures stop the job and a recovery run skips versions already accepted.

For the first publication, the operator supplies the environment secret `CARGO_REGISTRY_TOKEN`. When that secret is absent, the job uses `rust-lang/crates-io-auth-action` v1.0.6, pinned to `3268eb413133e2960fa4ae0e963ca76380c54cd9`, and passes its `token` output only to the publish step. After both crates exist, configure a trusted publisher on each crate with owner `VMAFx`, repository `vmafx`, workflow `supply-chain.yml` and environment `release-publish`, then remove the bootstrap secret. The workflow never prints either token. Gitea does not run the GitHub-only publisher.

Rust CI runs `cargo publish --dry-run --locked -p vmafx-sys -p vmafx -j4` immediately after the package check, without publication secrets. Both rehearsal and release verification use `DOCS_RS=1` to generate FFI from packaged headers, with libclang and no installed native library or GPU SDK required. Crate versions remain independent of the product release version and are never reset by release-please.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| :--- | :--- | :--- | :--- |
| Publish by hand | No workflow changes | Separate, unrehearsed publication path | Rejected |
| Trusted publishing from the first upload | No bootstrap secret | crates.io requires an existing crate | Unavailable |
| Protected release workflow with bootstrap secret, then OIDC | Rehearsed, tag-bound, idempotent publication | One-time publisher setup for both crates | Chosen |

## Consequences

- Release tags publish only missing crate versions, including partial-publication recovery; unchanged crate versions are skipped.
- Initial publication depends on the protected environment secret; subsequent publication depends on each crate's trusted-publisher configuration.
- `scripts/release/tests/test-publication-environment-binding.sh` refuses a missing crate publisher or one without `release-publish` and includes planted negative fixtures.

## References

- Implementation: `.github/workflows/supply-chain.yml`, `.github/workflows/rust-ci.yml`, both crate manifests, and `scripts/release/tests/test-publication-environment-binding.sh`.
- User guide: [Rust bindings](../api/rust.md#publishing).
- [crates.io trusted publishing](https://crates.io/docs/trusted-publishing).
- [Cargo publish reference](https://doc.rust-lang.org/cargo/commands/cargo-publish.html).
