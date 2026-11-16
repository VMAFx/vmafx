<!-- markdownlint-disable MD013 MD060 -->
# ADR-3555: Pin Intel NEO and Level Zero release asset URLs and SHA-256 digests in a lock file

- **Status**: Accepted
- **Date**: 2026-11-16
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `docker`, `release`, `supply-chain`, `sycl`, `intel`, `renovate`

## Context

`dev/scripts/fetch-intel-neo.py` resolved the matched Intel NEO compute-runtime stack ([ADR-1145](1145-neo-stack-derived-from-release.md)) and Level Zero loader packages ([ADR-1368](1368-oneapi-release-image-debian13.md)) at build time by making three `api.github.com` requests per run:

1. `repos/intel/compute-runtime/releases/tags/${INTEL_NEO_VERSION}` for `.deb` asset URLs and the release `.sum` file;
2. `repos/intel/intel-graphics-compiler/releases/tags/${IGC_TAG}` for the IGC `.deb` SHA-256 digests in the release body;
3. `repos/oneapi-src/level-zero/releases/tags/v${LEVEL_ZERO_VERSION}` for the `libze1` and `libze-dev` URLs and GitHub asset digests.

`.github/workflows/release-dry-run.yml` ([ADR-1595](1595-pr-time-verify-push-only-workflows.md)) intentionally holds no credentials or secrets (`test_is_a_dry_run` in `scripts/ci/tests/test_pr_time_verify_workflows.py` forbids `secrets.` across the workflow). Hosted runner builds of `docker/Dockerfile.production-gpu` in the dry run therefore called `api.github.com` anonymously and hit the shared 60-requests-per-hour IP rate limit.

Both upstream release tags are already pinned in `build-config.env` (`INTEL_NEO_VERSION`, `LEVEL_ZERO_VERSION`) and tracked by Renovate custom managers in `renovate.json`. Only the derived `.deb` filenames, `github.com` release-download URLs, and SHA-256 digests were resolved at build time.

## Decision

1. **Asset lock file (`dev/scripts/intel-neo-lock.json`)**:
   - Record `neo_version`, `level_zero_version`, `level_zero_distro` (`"u24.04"`), the six NEO role records under `neo` (`gmm`, `icd`, `level_zero`, `igc_core`, `igc_opencl`, `ocloc`), and the two loader records under `level_zero` (`libze1`, `libze-dev`).
   - Each asset record contains exact keys `name`, `url` (`https://github.com/<owner>/<repo>/releases/download/<tag>/<name>` only), and `sha256` (64 lowercase hexadecimal characters).
   - Reuse `StackAssets.select` (`runtime` and `ocloc`) and `level_zero_packages` (`libze1`, plus `libze-dev` with `--level-zero-dev`) so package selection matches the prior build-time resolver identically.
2. **Offline-metadata default mode (`dev/scripts/fetch-intel-neo.py`)**:
   - Default mode reads `dev/scripts/intel-neo-lock.json` next to the script (overridable via `--lock PATH`), performs zero `api.github.com` requests, ignores `--github-token`, downloads selected `.deb` archives directly from `https://github.com/...` through the existing HTTPS redirect boundary, verifies each file's SHA-256 against the lock, and writes `SHA256SUMS`.
   - Fail closed naming both the lock path and the `--write-lock` refresh command if the lock is missing, unreadable, malformed, contains duplicate or unknown keys, uses a non-`github.com` release-download URL or non-64-lowercase-hex digest, or disagrees with `--neo-ver`, `--level-zero-ver`, or `LEVEL_ZERO_DISTRO`.
   - Retain the optional `--github-token` flag and BuildKit `github_token` secret plumbing ([ADR-1271](1271-neo-buildkit-github-token-secret.md)) so existing callers remain compatible while lock mode ignores the token.
3. **Lock refresh mode (`--write-lock PATH`) and Renovate branch automation**:
   - `--write-lock PATH` runs the existing ADR-1145 and ADR-1368 resolution functions against `api.github.com` (using `--github-token` when supplied), verifies the resulting schema, and atomically writes sorted JSON without downloading `.deb` archives.
   - `.github/workflows/intel-neo-lock-refresh.yml` runs on `push` to `renovate/**` when `build-config.env` changes, with top-level `permissions: contents: read` and commit-pinned actions. It mints the release-bot App installation token (`RELEASE_BOT_APP_ID` / `RELEASE_BOT_PRIVATE_KEY`) as `.github/workflows/docs-render-pr.yml` does, runs `--write-lock dev/scripts/intel-neo-lock.json`, and commits and pushes the lock as the App only when the file changed.
4. **Callers, routing, and version-drift gate**:
   - `scripts/ci/install-intel-ocloc.sh` ([ADR-1360](1360-sycl-aot-compile-time-device-codegen.md), [ADR-1368](1368-oneapi-release-image-debian13.md)), `dev/Containerfile`, `Containerfile.vmafx`, `docker/Dockerfile.node`, `docker/Dockerfile.production-gpu`, and `docker/Dockerfile.tester` copy or pass `dev/scripts/intel-neo-lock.json` alongside `dev/scripts/fetch-intel-neo.py`.
   - `.github/ci-impact.json` (`tester_image`) and `scripts/ci/release-dry-run-plan.sh` (`gpu`) include `dev/scripts/intel-neo-lock.json` so lock edits route the same image checks as fetcher edits.
   - `scripts/ci/check-workflow-versions.py` (`Pre-Commit` job in `.github/workflows/lint-and-format.yml`, via `scripts/ci/check-base-image-single-source.sh`) fails closed when `dev/scripts/intel-neo-lock.json` is missing or its `neo_version` or `level_zero_version` differs from `build-config.env`, catching an unrefreshed version bump before image builds run.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Committed asset lock file refreshed by `--write-lock` and a `renovate/**` push workflow (chosen)** | Zero `api.github.com` calls during builds; `release-dry-run.yml` stays credential-free; every `.deb` URL and SHA-256 is pinned in Git and verified; Renovate bumps refresh automatically | Adds one JSON lock file and one narrow `renovate/**` workflow | Eliminates anonymous API rate-limit failures while preserving ADR-1145 matched-set derivation and ADR-1595 credential isolation |
| Pass `GITHUB_TOKEN` as a BuildKit secret in `release-dry-run.yml` | No lock file or refresh workflow | Violates [ADR-1595](1595-pr-time-verify-push-only-workflows.md) and `test_is_a_dry_run` in `scripts/ci/tests/test_pr_time_verify_workflows.py`, which forbid credentials and `secrets.` in the dry run | Rejected to keep the release dry run strictly credential-free |
| Renovate `postUpgradeTasks` running `--write-lock` | Single commit from Renovate without a follow-up workflow | The fork uses the Mend-hosted Renovate GitHub App ([`docs/development/dependency-bot.md`](../development/dependency-bot.md), line 4); Renovate's configuration-options documentation (`postUpgradeTasks`) states that commands stay blocked by default on Mend-hosted apps and `allowedCommands` is a global-only self-hosted configuration option | Rejected because Mend-hosted Renovate cannot execute repository `postUpgradeTasks` |
| Vendor `.deb` packages directly in Git | Fully offline archive fetch | Exceeds 100 MB of binary `.deb` artifacts per NEO release and bloats clone history ([ADR-1145](1145-neo-stack-derived-from-release.md)) | Rejected on repository size and binary-artifact policy |

## Consequences

- **Positive**:
  - Container builds and `scripts/ci/install-intel-ocloc.sh` never contact `api.github.com`, eliminating anonymous GitHub API rate-limit failures in `release-dry-run.yml` and local unauthenticated builds.
  - Every Intel NEO, IGC, gmmlib, `ocloc`, and Level Zero `.deb` filename, `github.com` release-asset URL, and SHA-256 digest is auditable in Git history.
  - `Pre-Commit` fails immediately when `INTEL_NEO_VERSION` or `LEVEL_ZERO_VERSION` moves without a matching lock update.
- **Negative**:
  - Manual edits to `INTEL_NEO_VERSION` or `LEVEL_ZERO_VERSION` in `build-config.env` require running `fetch-intel-neo.py --write-lock dev/scripts/intel-neo-lock.json` in the same branch; the error message and maintainer docs print the exact command.
- **Neutral / follow-ups**:
  - Amends [ADR-1145](1145-neo-stack-derived-from-release.md) and [ADR-1368](1368-oneapi-release-image-debian13.md) by moving GitHub Releases API resolution from image build time to `--write-lock` lock-refresh time.

## References

- Order `vmafx-release-dry-run-gpu-secret-adr1595` (2026-11-16, paraphrased): keep `release-dry-run.yml` credential-free under ADR-1595 and replace build-time anonymous `api.github.com` lookups in `dev/scripts/fetch-intel-neo.py` with `dev/scripts/intel-neo-lock.json`, refreshed via `--write-lock` and a `renovate/**` push workflow.
- [ADR-1145](1145-neo-stack-derived-from-release.md), [ADR-1271](1271-neo-buildkit-github-token-secret.md), [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md), [ADR-1368](1368-oneapi-release-image-debian13.md), [ADR-1595](1595-pr-time-verify-push-only-workflows.md).
- [`docs/development/dependency-bot.md`](../development/dependency-bot.md) (Mend-hosted Renovate GitHub App configuration).
