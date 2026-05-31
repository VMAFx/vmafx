<!-- markdownlint-disable MD013 MD060 -->
# ADR-0698: VMAFX Production Dockerfile — Multi-Arch, Image Signing, SBOM

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `docker`, `ci`, `release`, `security`, `sbom`, `signing`, `vmafx`, `fork-local`

## Context

The VMAFX rebrand (ADR-0686) requires a production-grade container image separate from
the development MCP container (`dev/Containerfile`). The dev container bakes in every
GPU SDK, the full oneAPI toolchain, and the NVIDIA Container Toolkit runtime — making it
unsuitable for production deployments where image size, attack surface, and
reproducibility matter.

A production image must satisfy four requirements:

1. **Slim runtime.** No build tools, no GPU SDKs, no Python interpreter above what the
   MCP server needs. Target: under 150 MB for the default CPU CLI variant.
2. **Multi-arch.** VMAFX runs on amd64 (x86\_64 servers, cloud VMs) and arm64 (Apple
   Silicon dev machines, AWS Graviton, Ampere A1). Both must ship from the same CI
   workflow with identical binaries.
3. **Signed + auditable.** Every image release must carry a Sigstore keyless cosign
   signature and a CycloneDX SBOM attached as a cosign attestation, so consumers can
   verify provenance without trusting the registry.
4. **GPU opt-in.** CUDA / ROCm / SYCL / Vulkan runtimes are large and platform-specific.
   The default image ships CPU-only; GPU-augmented variants are separate tags so users
   never pay for GPU runtime overhead they do not need.

## Decision

We will ship two Dockerfiles:

- `docker/Dockerfile.production` — CPU-only, multi-stage, dual-target (`--target cli`
  and `--target server`). Builder stage uses `ubuntu:24.04`; runtime stage uses
  `gcr.io/distroless/cc-debian12` (glibc, no shell). Python venv stage uses
  `python:3.13-slim` for the MCP server target.
- `docker/Dockerfile.production-gpu` — same structure, parametrized into five build
  targets (`final-cpu`, `final-cuda12`, `final-rocm6`, `final-oneapi2026`,
  `final-vulkan`). GPU runtime libs are copied from their upstream base images rather
  than installing entire SDKs.

A new workflow `.github/workflows/docker-publish-production.yml` fires on `v*` tag
pushes (release-please releases) and `workflow_dispatch`. It:

- Builds amd64 + arm64 for the CPU / Vulkan / server variants (QEMU emulation).
- Builds amd64-only for CUDA 12, ROCm 6, and oneAPI 2026 (GPU runtimes not portable
  to arm64 today; see Consequences).
- Signs every pushed digest via `cosign sign --yes` (Sigstore keyless OIDC).
- Generates a CycloneDX SBOM via `syft` and attaches it as a `cosign attest` predicate.
- Runs a smoke-test job that pulls the CPU CLI image and asserts `--version` exits 0.

Tag matrix:

| Tag suffix        | Platforms       | Description                            |
|-------------------|-----------------|----------------------------------------|
| *(none)*          | amd64, arm64    | CPU-only CLI (default, smallest)       |
| `-server`         | amd64, arm64    | CPU CLI + vmaf-mcp + vmaf-tune venv    |
| `-cuda12`         | amd64           | CUDA 12 runtime added                  |
| `-rocm6`          | amd64           | ROCm 6 HIP runtime added               |
| `-oneapi2026`     | amd64           | Intel oneAPI 2026 SYCL runtime added   |
| `-vulkan`         | amd64, arm64    | Vulkan ICD loader added                |

Base image rationale: `gcr.io/distroless/cc-debian12` was chosen over
`chainguard/wolfi-base` and `ubuntu:24.04` because it provides glibc (required by
libvmaf.so), has no shell (reducing attack surface — no bash, no apt), and produces
the smallest final layer footprint for a compiled C binary. Wolfi was considered but
requires a separate package repo configuration; distroless is directly supported by
cosign/syft tooling and maps cleanly to the Sigstore supply-chain story.

<!-- TODO: link parent cloud-native umbrella ADR once its number is allocated by the
     sibling agent. Parent PR: #1546 (VMAFX rebrand umbrella, ADR-0686). -->

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `ubuntu:24.04` runtime stage | Familiar; `apt-get` available for debugging | ~75 MB base; shell present (attack surface); users apt-install random deps | Distroless preferred per supply-chain policy |
| `chainguard/wolfi-base` | ~20 MB; glibc; regularly patched | Requires Chainguard package repo for system libs; less tooling support in cosign/syft ecosystem today | Distroless is simpler and better integrated |
| Single Dockerfile + ARG GPU\_RUNTIME (conditional COPY) | One file to maintain | BuildKit does not skip unused multi-stage chains from a single `FROM` — a `cuda12` build still pulls the ubuntu CUDA toolkit layer even when not selected | Two Dockerfiles (`production` + `production-gpu`) are cleaner |
| Parametrize GPU SDK install via ARG in builder stage | Fewer files | Builder stage must install GPU SDK at build time; CI runners have no GPU SDK by default and the apt install for cuda-toolkit-12-0 alone exceeds 3 GB | COPY runtime libs from upstream base images is smaller + faster |
| Arm64 for CUDA/ROCm/SYCL | Full parity | CUDA for arm64 requires `aarch64-linux-gnu` toolkit; ROCm arm64 not officially distributed; oneAPI arm64 not supported | Documented limitation; revisit when upstream distributes arm64 GPU runtimes |

## Consequences

- **Positive**: reproducible, signed, SBOM-attached production images on every release.
  Consumers can `cosign verify-attestation` to confirm the SBOM was built by CI. Image
  size for the CPU CLI target is expected to be 80–150 MB (distroless base ~20 MB +
  libvmaf.so ~8 MB + vmaf binary ~4 MB + models ~60 MB).
- **Positive**: arm64 support unblocks Apple Silicon and AWS Graviton deployments.
- **Negative**: GPU variant arm64 support is deferred. The `-cuda12`, `-rocm6`, and
  `-oneapi2026` tags are amd64-only until their upstream distributions ship arm64
  packages (CUDA / ROCm / oneAPI all have amd64-first release cadences).
- **Neutral**: `docker/Dockerfile.production` is fork-only and does not conflict with
  upstream Netflix/vmaf, which ships no production Dockerfile.
- **Follow-up**: add a pixel-level golden-score assertion to the CI smoke-test once the
  YUV test fixtures are mounted into the GitHub Actions runner (requires a separate
  artifact-cache or fixture-embed step). The current smoke-test only checks `--version`.
- **Follow-up**: add `T-DOCKER-SMOKE` to the "Recently closed" block in `docs/state.md`
  once the smoke-step has been green for three consecutive master runs (per the existing
  deferred entry).

## References

- ADR-0686: VMAFX rebrand umbrella — `docs/adr/0686-vmafx-rebrand-aggressive-modernization.md`
- `dev/Containerfile` — development / MCP container (separate, not replaced)
- `docker/Dockerfile.production` — this ADR's primary deliverable
- `docker/Dockerfile.production-gpu` — GPU-augmented variant
- `.github/workflows/docker-publish-production.yml` — CI publish workflow
- `docs/development/docker-production.md` — operator guide
- Sigstore cosign: <https://docs.sigstore.dev/cosign/signing/overview/>
- syft: <https://github.com/anchore/syft>
- distroless cc-debian12: <https://github.com/GoogleContainerTools/distroless>
- PR: `feat(docker): production multi-arch Dockerfile + image signing + SBOM`
- Parent PR: #1546 (VMAFX rebrand umbrella)
- Source: user direction (VMAFX Phase 3B brief, 2026-05-28)
