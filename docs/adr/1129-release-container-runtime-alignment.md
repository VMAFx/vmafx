<!-- markdownlint-disable MD013 MD060 -->
# ADR-1129: Align release containers with the published tag and runtime ABI

- **Status**: Accepted
- **Date**: 2026-08-31
- **Deciders**: Lusoris, Codex (OpenAI)
- **Tags**: release, containers, supply-chain, security, mcp, go, gpu, ci

## Context

The first independent VMAFx patch release publishes seven container images in
addition to the native and Python artifacts governed by
[ADR-1127](1127-single-semver-release-stream.md). The old Docker workflows ran
on any `v*` tag push, mixed a release tag with implicit checkout state, treated
several GPU SBOM operations as best effort, and used success-masked post-push
probes. A green release could therefore describe one source revision while
shipping an unsigned, unstartable, or differently-versioned image.

The image definitions also crossed unsupported runtime boundaries. Debian 13
builders fed Debian 12 runtimes, a Python virtualenv was copied without its
matching interpreter, GPU backends were built outside their vendors' complete
runtime families, and the node image copied x86-specific FFmpeg libraries into
a nominally multi-architecture target. Operator and node builds injected a
nonexistent `main.buildVersion` symbol and then started long-running processes
for their smoke probes, so neither image exposed reliable release-version
truth.

The Python server had a second release blocker: its declared MCP 2.1 dependency
removed the decorator and request-context interfaces used by the implementation.
The production server entrypoint selected HTTP while the image omitted the
`[http]` dependencies and the CLI no longer dispatched to that transport.
Container repair, release identity, and post-push verification must move
together because fixing only one layer still permits a broken release.

## Decision

VMAFx container publication will be one tag-bound, fail-closed contract. Both
Docker workflows will run on `release.published`, check out
`github.event.release.tag_name`, and derive every release image tag from that
same value. CPU and node builds will use a Debian 13 ABI through their Debian 13
distroless runtime; the server will retain the exact pinned Python 3.14 base
used to build its virtualenv; and CUDA 13.3.1, ROCm 7.2.4, and oneAPI 2025.3.1
images will use digest-pinned, version-matched vendor build/runtime families.
Every final image will run as UID/GID 65532. Operator and node builds will
inject the release tag into `pkg/version.version` and expose a non-blocking
`--version` path. The operator image and Helm chart will share the env-only
ADR-1119 contract, with metrics on `:8080` and health/readiness on `:8081`; the
chart will not pass the removed pre-fx CLI flags, and its default Go server,
operator, and node image references will match the canonical `vX.Y.Z` tags
published by the release workflows. The MCP server will use the
typed MCP 2.x constructor
handlers, preserve tool-error and progress-session semantics, install
`[eval,http]`, and opt only the container into all-interface HTTP binding while
keeping bearer authentication fail closed. Each of the seven pushed image
digests must receive a cosign signature, GitHub-native build provenance, a Syft
CycloneDX SBOM and cosign SBOM attestation; a dependent smoke job must verify
the signature before pulling and exercising the published runtime.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Patch only the first failing Docker command | Small diff and short build | Leaves source/tag ambiguity, ABI crossings, missing signatures, and success-masked smokes | A partial repair can still publish a green but unusable release |
| Keep one distroless base and copy selected Python/GPU libraries into it | Small final images and one runtime policy | Virtualenv interpreters and accelerator runtimes are not self-contained library bundles; missing transitive ABI dependencies surface only at startup | The prior design already demonstrated these unsupported boundaries |
| Build every variant in one universal SDK image | One Dockerfile path and fewer stages | Very large images, conflicting vendor stacks, larger CVE surface, and unnecessary toolchains at runtime | Runtime images should contain one coherent backend family |
| Pin MCP below 2.x | Avoids the registration migration | Contradicts the declared dependency and postpones an unavoidable public SDK migration | The typed MCP 2.x constructor API is available and directly testable |
| Match each artifact to its release tag and runtime family, then verify the pushed digest | Immutable source identity, complete runtime dependencies, auditable provenance, meaningful smokes | More build time, larger vendor images, and more release jobs | Chosen; release correctness and recoverability outweigh the additional CI cost |

## Consequences

- **Positive**: an arbitrary `v*` tag push no longer publishes production
  images; the authenticated GitHub release is the publication authority and
  source checkout, image tag, Go version output, signatures, attestations, and
  smoke inputs share one release identity.
- **Positive**: local builds proved the CPU CLI, Python server, all three GPU
  production variants, operator, and amd64 node start as UID/GID 65532 with
  their intended entrypoints and runtime dependencies.
- **Positive**: the Helm operator Deployment now configures the release binary
  through supported environment variables and probes the ports on which the
  process actually listens.
- **Positive**: MCP tool registration now follows the installed 2.1 API,
  returns typed `CallToolResult(isError=True)` failures, translates wire-format
  progress tokens, and restores the documented authenticated HTTP surface.
- **Negative**: ROCm and the other vendor-complete runtime images are larger
  than copying a few shared libraries into distroless, and QEMU arm64 node
  builds remain expensive.
- **Negative**: GPU release smoke tests validate signature, pull, dynamic
  linkage, and the device-independent `--version` path without accelerator
  hardware. Backend execution and numeric parity remain separate device-runner
  gates.
- **Negative**: the server's `[http]` dependency ranges are resolved at image
  build time rather than from a Python lockfile. The generated SBOM records the
  concrete result, but a later rebuild can resolve newer compatible packages.
- **Neutral / follow-ups**: vendor version and digest bumps must update builder,
  runtime, docs, and smoke expectations together. The first real `v3.2.1`
  publication is the end-to-end OIDC/GHCR proof; local validation cannot mint
  GitHub or Sigstore identities.

## Supply-chain impact

- **Release authority**: `release.published` replaces arbitrary tag pushes for
  production publication. Manual dispatch remains a development/recovery path
  and does not redefine the authenticated release event.
- **New runtime dependencies**: the server installs the existing optional
  `vmaf-mcp[http]` surface (`aiohttp>=3.9.0`, Apache-2.0; and
  `prometheus-client>=0.20.0`, Apache-2.0) in addition to `[eval]`. These enable
  the already-documented HTTP listener, health/readiness routes, and metrics.
- **Build/runtime images**: Debian 13 slim/distroless, Python 3.14 slim, NVIDIA
  CUDA 13.3.1, AMD ROCm 7.2.4, and Intel oneAPI 2025.3.1 references are pinned
  by digest. The Go operator remains a static binary; the node moves its CGO
  chain to Debian 13 end to end.
- **Build-time fetches**: CUDA installs `nv-codec-headers` at exact commit
  `876af32a202d0de83bd1d36fe74ee0f7fcf86b0d`; the commit is checked after the
  bounded fetch before installation. This replaces distro headers that lack
  loader symbols required by the source.
- **Attestation**: all five production-image jobs and both operator/node jobs
  use SHA-pinned `actions/attest-build-provenance` v4.2.2, cosign 3.1.3 keyless
  signing, and digest-addressed subjects. Smoke jobs verify the expected
  workflow identity before pulling.
- **CVE/exposure delta**: matching vendor images widen the packaged runtime
  relative to selective library copies but remove unknown missing-dependency
  behavior. Every final process drops root. The server deliberately binds
  `0.0.0.0:8080` only in the container and retains mandatory bearer
  authentication unless an operator explicitly selects the documented
  no-auth escape hatch.

## SBOM delta

The old path generated useful CPU/operator/node inventories but allowed GPU
SBOM generation to fail without failing the release and did not cover the
published server uniformly. The accepted workflow produces one CycloneDX JSON
document for each of these digest subjects:

1. CPU CLI;
2. CUDA 13.3.1;
3. ROCm 7.2.4;
4. oneAPI 2025.3.1;
5. Python 3.14 MCP server;
6. Go operator; and
7. Go/FFmpeg node.

Each document is uploaded as workflow evidence and attached to the image via a
cosign CycloneDX attestation; generation and attachment are hard failures. The
server inventory gains its resolved `aiohttp` and `prometheus-client`
dependency closure. GPU inventories now contain their complete vendor runtime
families. The node inventory becomes architecture-correct because FFmpeg's
non-glibc dependency closure is collected by native `ldd` output rather than
hard-coded `/usr/lib/x86_64-linux-gnu` paths. The separate
`supply-chain.yml` continues to own signed SPDX/CycloneDX inventories for native
release files and `vmaf-mcp` distributions, so container and downloadable
artifacts have complementary rather than duplicated SBOM authorities.

## References

- [ADR-1127](1127-single-semver-release-stream.md)
- [ADR-0902](0902-signing-and-attestation-audit.md)
- [Research-1129](../research/1129-release-container-runtime-alignment.md)
- [GitHub Actions release event](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release)
- [MCP Python SDK v2.1.1](https://github.com/modelcontextprotocol/python-sdk/tree/v2.1.1)
- [SLSA and Sigstore release guidance](../development/release.md)
- Source: `req` — "get everything open merged (the pr's), then fix ci so that it is green, fix the tags and then bump a release"
- Source: `req` — "all this will be a .x patch of course, not a minor version"
