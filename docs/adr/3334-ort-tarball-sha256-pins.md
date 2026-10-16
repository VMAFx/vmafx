<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3334: ONNX Runtime C Tarball SHA-256 Pinning and Verification

- **Status**: Accepted
- **Date**: 2026-10-16
- **Deciders**: Lusoris
- **Tags**: ci, security, supply-chain

## Context

ONNX Runtime's C/C++ native release tarballs are downloaded and extracted during container builds (`dev/Containerfile`) and across multiple CI workflow jobs (`go-ci.yml`, `build.yml`, `libvmaf-build-matrix.yml`, and `tests-and-quality-gates.yml`). While `.github/workflows/build.yml` previously introduced an explicit SHA-256 checksum verification step before tarball extraction, the remaining sites downloaded the release archive directly from GitHub Releases via `curl` and extracted it without cryptographic verification of the archive contents.

Under HISS-11 (Hermetic Supply Chain), every external dependency artifact downloaded across automated infrastructure must have its integrity validated against an immutable cryptographic digest prior to execution or extraction. Furthermore, in cached environments such as the GPU coverage runner in `tests-and-quality-gates.yml`, local cache pollution or corrupted downloads could persist unverified. A consistent mechanism was required to pin digests at every site, ensure automated maintenance via Renovate without version/digest drift, and fail CI immediately if any download site lacks checksum enforcement.

## Decision

We will pin exact SHA-256 digests (`ORT_SHA256`) on every ONNX Runtime C tarball download across container definitions and CI workflows, verifying the archive with `sha256sum --check --strict` prior to extraction. We will validate all download sites via a new static check (`scripts/ci/check_ort_tarball_pins.py`) enforced in pre-commit and CI, and configure Renovate's `github-release-attachments` regex manager to keep `ORT_VERSION` and `ORT_SHA256` synchronized in locked pairs.

Specifically:

1. Every site downloading from `onnxruntime/releases/download` defines `ORT_SHA256` directly adjacent to `ORT_VERSION`.
2. Every site validates the downloaded archive using `sha256sum --check --strict` before invoking `tar -x`.
3. Cached tarball flows (such as the GPU leg in `tests-and-quality-gates.yml`) verify the checksum on every run including cache hits, purging the cached file and failing on mismatch.
4. Static enforcement (`scripts/ci/check_ort_tarball_pins.py` and unit tests in `scripts/ci/tests/test_ort_tarball_single_source.py`) rejects missing pins, malformed hex digests, extractions occurring before verification, and checksum discrepancies across sites sharing identical flavour and version pairs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| (a) Per-site `ORT_SHA256` + `github-release-attachments` | Co-locates version and digest directly with the step; Renovate updates both atomically via regex; static scanner verifies all sites without centralized coupling. | Slight duplication across unmanaged legacy pin sites. | **Chosen.** Satisfies HISS-11 with minimum build plumbing and clean automated updates. |
| (b) Single source in `build-config.env` read by every site | Centralizes values into one file. | Containerfile and disparate workflow runners would require extra environment sourcing plumbing; Renovate cannot update two adjacent variables in `build-config.env` via a single regex manager without custom multi-value support. | Rejected due to plumbing complexity and updater toolchain friction. |
| (c) No hash, rely on TLS | Zero maintenance overhead. | Fails HISS-11; transport security does not prevent upstream release asset substitution, CDN poisoning, or compromised release artifacts. | Rejected as insecure. |

## Consequences

- **Positive**: Full cryptographic supply-chain integrity for ONNX Runtime binaries across all builds and tests; Renovate automatically bumps version and hash together on managed sites; static check blocks PRs introducing unpinned or unverified tarball downloads.
- **Negative**: Manual updates required when intentionally updating legacy fixed lines (`1.22.0`).
- **Neutral / follow-ups**: Enforced by pre-commit hook `test-base-image-single-source` and CI job `release-script-contract` in `rule-enforcement.yml`.

## References

- Q-controller ledger vmafx-draft-verdicts-1232 (paraphrased): pin the ORT C tarball sha256 with a planted wrong-hash case.
- See [ADR-0113](0113-ort-create-session-fallback-multi-ep-ci.md) and [ADR-0120](0120-ai-enabled-ci-matrix-legs.md) for DNN runtime decoupling and legacy version pinning.
- See [ADR-1134](1134-vmafx-ort-runner-in-tree.md) for Go CI ONNX Runtime dependencies.
- See [ADR-1231](1231-base-image-single-source.md) for base image single-source conventions.
