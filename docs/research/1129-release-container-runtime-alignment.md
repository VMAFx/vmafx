<!-- markdownlint-disable MD013 MD060 -->
# Research-1129: Release container runtime alignment

## Question

What coupled release, runtime, MCP, and verification changes are required for
`v3.2.1` to publish containers that actually represent and run the source named
by the release?

## Method

The audit compared the two Docker publication workflows, five production
Dockerfiles, Go entrypoints, and the installed MCP 2.1 API. It then built the
reachable final images from their exact digest-pinned bases, inspected their
configured users and entrypoints, ran their device-independent runtime probes,
and exercised the Python server through its authenticated HTTP surface. Static
workflow evidence was checked separately from operations that require GitHub
OIDC or writes to GHCR.

## Evidence

### Release identity was not a single value

- The prior Docker workflows accepted every `v*` tag push and derived metadata
  from the triggering ref while checkout otherwise relied on implicit event
  state. That made an arbitrary tag push a production-publishing authority and
  left manual and release paths with different tag construction.
- Publishing the release-please draft produces a `release.published` event whose
  GitHub contract supplies the release tag ref and tagged commit. The repaired
  workflows explicitly check out `github.event.release.tag_name`, use that same
  value for all release image tags, and group concurrency by the release.
- Manual recovery previously accepted an arbitrary image-tag input while every
  build checked out the independent dispatch ref. A run from `master` with
  input `v3.2.1` could therefore publish and attest branch source under the
  release name. The shared preflight contract now requires one existing
  published ordinary tag to match the input, `GITHUB_REF`, `GITHUB_SHA`,
  checkout, and every coordinated version before any image build can start.
- The Go server, operator, and node Docker builds now receive that value as
  `VMAFX_VERSION`, inject it into
  `github.com/VMAFx/vmafx/pkg/version.version`, and answer `--version` before
  starting Kubernetes, gRPC, or Fx lifecycles. The previous ldflag targeted a
  nonexistent `main.buildVersion` symbol.

### Helm validation executed an unverified moving installer

- Both Helm workflows pinned the intended binary version but downloaded the
  installer script from `helm/helm`'s moving `main` branch and piped the
  response directly into Bash. The source that selected and installed the
  binary was therefore neither immutable nor authenticated by the version pin.
- The replacement downloads `helm-v4.2.4-linux-amd64.tar.gz` as a file from
  Helm's official distribution endpoint, checks the published SHA-256
  `c306b46f719b0a4da32d0f78ee21bf90ce8d602f15b22ab753f0674d1670a7f3`, and
  extracts only after verification. The chart and Kubernetes E2E workflows
  keep the version, digest, and install sequence identical.
- The same audit found the E2E workflow assigning an unused `rc` from a file
  that no step created. Because the kuttl step uses `continue-on-error` to
  preserve diagnostics, a command failure without an XML failure marker could
  be reported green. The final assertion now reads the step's raw `outcome`
  and fails on every value other than `success` after evidence uploads finish.

### Build and runtime families crossed unsupported ABI boundaries

- The CPU, Go server, and node paths crossed or depended on an external runtime
  ABI. They now build the fork's own libvmaf and run against Debian 13, with the
  final distroless image's nonroot UID/GID 65532.
- A CPython virtualenv contains an interpreter link, not a portable interpreter
  and standard library. The old server copied a virtualenv and selected Python
  libraries into a non-Python base. The repaired server uses the identical
  digest-pinned `python:3.14-slim` image for dependency construction and final
  runtime, then drops to UID/GID 65532.
- CUDA, ROCm, and oneAPI cannot be reduced safely to a few hand-selected shared
  objects copied from a different base. The production GPU targets now pair
  NVIDIA CUDA 13.3.1 devel/runtime and Intel oneAPI 2025.3.1 basekit/runtime
  image families; AMD uses its official ROCm 7.2.4 development image, which
  also carries the complete matching runtime, for both stages. Every reference
  is pinned by SHA-256 digest.
- Ubuntu 24.04's Meson 1.3 accepts the C23 spelling `c2x`, and the GPU custom
  targets require a source-relative `core/build` directory. CUDA also needs
  loader definitions from exact `nv-codec-headers` commit
  `876af32a202d0de83bd1d36fe74ee0f7fcf86b0d`; the distro snapshot is older.
  These are build mechanics, not optional optimizations.
- The node image hard-coded `/usr/lib/x86_64-linux-gnu` codec copies while
  declaring an arm64 target. The repaired native FFmpeg stage uses `ldd` to
  collect its non-glibc dependency closure and therefore resolves the correct
  Debian multiarch directory on each build architecture.

### The published MCP server did not match its declared SDK or HTTP surface

- `mcp>=2.1.1` removed the low-level `Server.list_tools()` and
  `Server.call_tool()` decorator API and the old global request-context access.
  The repaired server passes typed `on_list_tools` and `on_call_tool` handlers
  to `Server(...)`, returns `ListToolsResult` / `CallToolResult`, maps dispatcher
  exceptions to `isError=True`, and exposes the request session through a
  task-local `ContextVar` only for an active tool call.
- Pydantic's `TypeAdapter[JSONRPCMessage]` now validates the MCP 2.x message
  union. A real `CallToolRequestParams` parse proved wire key
  `_meta.progressToken` normalizes to `params.meta["progress_token"]` and is
  handed to the dispatcher without leaking the request session afterward.
- The CLI again dispatches `--transport http` before entering stdio. The
  optional `[http]` extra declares `aiohttp` and `prometheus-client`, and the
  production image installs `[eval,http]` rather than only `[eval]`.
- Host installations retain a loopback default. The server image explicitly
  opts into `VMAFX_MCP_HTTP_BIND=0.0.0.0` so Docker port publication works, but
  the shared middleware still returns HTTP 401 without its configured bearer
  token.

### The Helm operator still deployed the removed pre-fx interface

- ADR-1119 made operator runtime configuration environment-only, and the
  installed golusoris v0.7.0 operator module defaults metrics to `:8080` and
  health/readiness to `:8081`. The chart still passed the removed
  `--metrics-bind-address`, `--health-probe-bind-address`, `--leader-elect`, and
  `--log-level` arguments while declaring ports `8081` and `8082`.
- The Go entrypoint ignored those stale arguments, so the process listened on
  its new defaults while Kubernetes probed `8082`. A syntactically valid Helm
  render could therefore produce a permanently unready release Pod.
- The repaired template uses the compound-key env contract, declares ports
  `8080` and `8081`, and keeps the named liveness/readiness probe wired to the
  health port. Its default Go server, operator, and node image references also
  carry the `v` prefix and repositories used by the release publishers rather
  than selecting unpublished images. The Dockerfile and all current operator
  guides now expose those same defaults; historical ADR and changelog records
  remain unchanged.

### Post-push checks previously proved too little

- The operator/node smoke steps ran `--help || true`, so a missing library,
  wrong entrypoint, or nonzero process could still print a success message.
  GPU SBOM paths also carried `continue-on-error` or a `pip install syft || true`
  fallback that did not install Anchore's Go binary reliably.
- Each of the five production and three Go-service image jobs now signs its
  pushed digest with cosign 3.1.3, emits GitHub-native build provenance through
  SHA-pinned `actions/attest-build-provenance` v4.2.2, generates a Syft
  CycloneDX document, attaches that document through cosign, and uploads it as
  workflow evidence. The scoped workflows contain no `continue-on-error` or
  `|| true` escape hatch.
- Dependent smoke jobs install cosign and verify the expected workflow identity
  against the digest before pulling. They then execute the CPU/GPU CLI, the
  live authenticated Python HTTP server, the live Go server health/readiness
  endpoints, or the exact operator/node version and node `vmaf`/FFmpeg
  entrypoints. Summary jobs require every build and smoke result to be
  `success`.
- The separate artifact workflow adds signed SPDX and CycloneDX inventories for
  the native files and the installed `vmaf-mcp` dependency closure. The
  container changes extend equivalent hard-failure coverage to all eight image
  digests rather than creating a second authority for downloadable files.

## Verified results

- CPU CLI: `docker build --target cli -f docker/Dockerfile.production ...`
  completed; `vmaf --version` returned `3.2.1`; configured and runtime identity
  were UID/GID 65532.
- Python server: the final image reported Python 3.14.7 and MCP 2.1.1 as
  UID/GID 65532. Its authenticated detached HTTP process listened on
  `0.0.0.0:8080`, returned `{"status":"healthy"}` and
  `{"status":"ready","vmaf_binary":"/usr/local/bin/vmaf"}`, and returned
  HTTP 401 without the token.
- GPU production targets `final-cuda13`, `final-rocm7`, and
  `final-oneapi2025` all built from the pinned vendor families, configured
  `/usr/local/bin/vmaf` as entrypoint, ran as `65532:65532`, and returned
  `3.2.1` from `--version` without a device.
- Operator: a build with `VMAFX_VERSION=v3.2.1` ran as UID 65532 and returned
  `v3.2.1`; `go test ./cmd/vmafx-operator` passed. A Helm render with
  `operator.enabled=true` contained no removed CLI flags, exported the four
  supported environment variables, and aligned the named metrics/health ports
  and probes to `8080`/`8081`.
- Go server amd64 and arm64: both release targets ran as UID/GID 65532 with
  `/usr/local/bin/vmafx-server` as the entrypoint, returned `v3.2.1`, and
  reported embedded libvmaf `3.2.1`. Detached runtime probes returned HTTP 200
  with `{"status":"ok"}` from `/healthz` and `{"status":"ready"}` from
  `/readyz`; the arm64 proof ran under QEMU from the loaded multi-platform
  image.
- Node amd64: the full `node-cpu` image ran as nonroot, returned `v3.2.1`,
  reported embedded libvmaf `3.2.1` and FFmpeg `n9.0.1-17-g8dcc9a8`, and
  `go test ./cmd/vmafx-node` passed in 0.122 seconds.
- Node arm64: the full QEMU build completed all VMAF, SVT-AV1, FFmpeg, and Go
  links with `--platform linux/arm64`; image inspection reported architecture
  `arm64`, user `nonroot:nonroot`, and entrypoint
  `/usr/local/bin/vmafx-node`. QEMU runtime probes returned node `v3.2.1`,
  libvmaf `3.2.1`, and FFmpeg `n9.0.1-17-g7aff8bd`, all with exit status zero.
- MCP: the production-runtime suite passed 460 tests with 2 skips in 1.92
  seconds; the focused installed-MCP adapter suite passed 5 tests in 0.32
  seconds.
- Static gates: actionlint accepted both workflows; hadolint accepted all four
  Dockerfiles; touched-file pre-commit hooks, Ruff, and `git diff --check`
  passed. No Netflix golden assertion changed.

The local run cannot mint a GitHub OIDC identity, push a production digest, or
prove GHCR's post-push attachment APIs. Those operations are deliberately left
to the tag-bound release jobs. Both amd64 and full QEMU arm64 Go-server and node
images were built and exercised locally; the workflow remains fail closed if
either architecture fails in the release environment.

## Result

Treat release identity, runtime-family matching, MCP 2.x compatibility, and
post-push evidence as one publication decision. A release is green only when
every required digest is built from the published tag, runs nonroot, is signed
and attested, has a generated SBOM, passes signature verification before pull,
and completes its device-independent runtime smoke.

## Reproducer

```bash
actionlint \
  .github/workflows/docker-publish-production.yml \
  .github/workflows/docker-publish-operator-node.yml

hadolint \
  docker/Dockerfile.production \
  docker/Dockerfile.production-gpu \
  docker/Dockerfile.operator \
  docker/Dockerfile.node \
  Dockerfile.go-server

docker build --target cli -f docker/Dockerfile.production \
  -t vmafx-release-alignment:cli .
docker run --rm vmafx-release-alignment:cli --version

docker build --target server -f docker/Dockerfile.production \
  -t vmafx-release-alignment:server .
docker run --rm --entrypoint /venv/bin/python \
  vmafx-release-alignment:server --version

for target in final-cuda13 final-rocm7 final-oneapi2025; do
  docker build --target "$target" -f docker/Dockerfile.production-gpu \
    -t "vmafx-release-alignment:$target" .
  docker run --rm "vmafx-release-alignment:$target" --version
done

docker build --target operator --build-arg VMAFX_VERSION=v3.2.1 \
  -f docker/Dockerfile.operator -t vmafx-release-alignment:operator .
docker run --rm vmafx-release-alignment:operator --version

docker build --target go-server --build-arg VMAFX_VERSION=v3.2.1 \
  -f Dockerfile.go-server -t vmafx-release-alignment:go-server .
docker run --rm vmafx-release-alignment:go-server --version

helm template vmafx deploy/helm/vmafx \
  --set operator.enabled=true \
  --show-only templates/operator-deployment.yaml

docker build --target node-cpu --build-arg VMAFX_VERSION=v3.2.1 \
  --build-arg VMAF_BUILD_JOBS=4 -f docker/Dockerfile.node \
  -t vmafx-release-alignment:node .
docker run --rm vmafx-release-alignment:node --version
docker run --rm --entrypoint /usr/local/bin/vmaf \
  vmafx-release-alignment:node --version
docker run --rm --entrypoint /usr/local/bin/ffmpeg \
  vmafx-release-alignment:node -version

go test ./cmd/vmafx-server ./cmd/vmafx-operator ./cmd/vmafx-node
(cd mcp-server/vmaf-mcp && python -m pytest -q)
```

## Sources

- [GitHub Actions release event](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release)
- [GitHub artifact attestations](https://docs.github.com/en/actions/security-for-github-actions/using-artifact-attestations)
- [Sigstore cosign container verification](https://docs.sigstore.dev/cosign/verifying/verify/)
- [Anchore Syft](https://github.com/anchore/syft/tree/v1.51.1)
- [MCP Python SDK v2.1.1](https://github.com/modelcontextprotocol/python-sdk/tree/v2.1.1)
- [NVIDIA CUDA container images](https://hub.docker.com/r/nvidia/cuda)
- [AMD ROCm Docker guidance](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/how-to/docker.html)
- [Intel oneAPI containers](https://www.intel.com/content/www/us/en/developer/articles/technical/containers.html)
- [ADR-1127 research](1127-single-semver-release-stream.md)
