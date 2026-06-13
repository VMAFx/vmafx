<!-- markdownlint-disable MD013 MD060 -->
# ADR-0717: vmafx-node — ffmpeg latest-tag pinning + ffmpeg-patches bundled into Dockerfile

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: node, ffmpeg, docker, phase4b, fork-local

## Context

Phase 4b.4 (ADR-0709) requires the `vmafx-node` worker container to run `ffmpeg` as a
subprocess for all encoding operations. The node image must therefore include a
fully-compiled `ffmpeg` binary with the fork's `ffmpeg-patches/` series applied (the
patches wire the VMAFX-specific libvmaf filters into FFmpeg's filter graph, expose the
SYCL / Vulkan / CUDA / HIP backend selectors, and add the vmaf-tune qpfile and
profile-glue patches).

Two packaging decisions require an explicit record:

1. **Which FFmpeg version to pin in the node image** — the options are a pinned stable
   release tag (e.g., `n8.2`) versus tracking a rolling release branch.
2. **Where the ffmpeg build lives** — the node Dockerfile vs a shared external image.

The existing `dev/Containerfile` (the developer container) already builds FFmpeg n8.1.1
with all 15 patches applied. That container is not a runtime delivery artifact; it exists
to give developers a full-featured workbench. The node Dockerfile needs its own
independent ffmpeg build so the production node image stays lean (no developer toolchain,
no Python environment, no MCP server).

## Decision

**Option A — pin to latest stable tagged release** is chosen.

The `docker/Dockerfile.node` pins `FFMPEG_TAG=n8.2` (the latest stable FFmpeg 8.x
release at the time of this ADR). The tag is a Docker build argument with a documented
default; CI can override it to test against a newer tag. The value is updated on every
release-sync PR that touches `ffmpeg-patches/`.

The Dockerfile structure is a shared multi-stage build:

1. **vmaf-builder** — compiles `libvmaf.so` + `vmaf` CLI from `core/` (CPU-only,
   release/stripped). Shared by all four node variants.
2. **ffmpeg-deps-base** — installs codec build-time dependencies (nasm, libx264-dev,
   libx265-dev, libvpx-dev, libdav1d-dev, libopus-dev, ...) and builds SVT-AV1 from
   source (Ubuntu's `libsvtav1-dev` omits `SvtAv1Enc.pc` — same root cause documented
   in the dev Containerfile's §"NOTE: libaom" comment block).
3. **ffmpeg-builder-cpu** — clones FFmpeg at `FFMPEG_TAG`, applies all patches in
   `ffmpeg-patches/series.txt` order via `git am --3way`, configures with
   `--enable-libvmaf --enable-libx264 --enable-libx265 --enable-libvpx
   --enable-libsvtav1 --enable-libdav1d`, and installs into `/ffmpeg-install`.
4. **go-builder** — compiles `cmd/vmafx-node` with CGo disabled for libvmaf linkage.
5. **runtime-base** — `gcr.io/distroless/cc-debian12` (same base as the controller and
   production images).
6. **node-cpu / node-cuda / node-rocm / node-sycl** — four final targets that add
   vendor-specific runtime libs on top of the shared base.

**libaom is excluded** for the same reason as in the dev container: `ffmpeg-patches/0007`
references `aom_roi_map_t` fields (`enabled`, `skip`, `ref_frame`, `delta_qp_enabled`)
that do not exist in any released libaom. SVT-AV1 covers the AV1 production lane.

The `pkg/encoder` package (already in tree at `pkg/encoder/encoder.go`) already shells
out to `ffmpeg` via `EncodeParams.FFmpegBin`. The node startup probe
(`cmd/vmafx-node/probe/probe.go`) runs `ffmpeg -encoders` at startup and caches the
result; the server logs the inventory on every start so operators can confirm the codec
matrix without shelling into the container.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A — pin to latest stable tag** (chosen) | Reproducible builds; `n8.2` tag is immutable; CI can test against a known state | Must update the tag on every release sync | Reproducibility outweighs the small maintenance cost. Rolling builds introduce non-determinism that is difficult to bisect when a codec behaviour changes. |
| **B — track release/8.x branch** | Always gets the latest patch-level fixes without a manual tag bump | Rolling: two builds of the same image can produce different binaries; ffmpeg-patches may break against a mid-branch commit | Violates the principle of immutable image content. Commit SHAs are acceptable; branch names are not. |
| **C — reuse the dev container ffmpeg** | Zero extra build time | The dev container is a 4GB+ developer workbench; pulling it at node-runtime time is impractical; the CUDA/oneAPI toolchains would inflate the production image by 2–3GB | Not a production delivery artifact; wrong base image. |
| **D — install distro ffmpeg via apt** | Zero build time | Ubuntu's ffmpeg package cannot have the fork's patches applied; lags the upstream release significantly; codec matrix is frozen to whatever the distro ships | The patches are load-bearing; distro ffmpeg cannot be used. |

## Consequences

**Positive:**

- Every `vmafx-node` pod runs the exact same ffmpeg binary with all 15 fork patches.
- The `pkg/encoder` package and `cmd/vmafx-node/probe` package are codec-inventory-aware
  at startup, enabling the controller to dispatch only to nodes that carry the requested
  codec.
- The four Docker targets (cpu / cuda / rocm / sycl) share the same ffmpeg binary via
  the common `ffmpeg-builder-cpu` stage; the vendor variants add only their GPU runtime
  libs on top.
- CI can validate the node image build with `docker buildx build --target node-cpu`.

**Negative:**

- Every node image build must compile ffmpeg from source (~5–10 min on standard runners).
  This can be accelerated with `--cache-from` in CI.
- When `ffmpeg-patches/` are updated (e.g., after an FFmpeg n8.3 base bump), the
  `FFMPEG_TAG` build-arg in `docker/Dockerfile.node` must be updated in the same PR
  (CLAUDE.md §12 r14).

**Neutral / follow-ups:**

- Phase 4b.5+ will add full gRPC service registration in `cmd/vmafx-node/server/server.go`
  once the controller proto is finalized.
- NVENC / QSV / AMF hardware encoders require the host's GPU driver; the node container
  does not bundle drivers. The startup probe logs WARN for absent hw-encoder codec names
  but does not block startup.
- A CI job that validates the patches apply against both `n8.1.1` (existing) and `n8.2`
  (new node target) is tracked as a follow-up to this PR; the existing
  `ffmpeg-patches/test/build-and-run.sh` gate covers n8.1.1, and the Dockerfile itself
  serves as a build-time n8.2 validation.

## References

- [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) — Phase 4b umbrella (parent
  ADR); item 4b.4 is the direct parent scope.
- [ADR-0541](0541-dev-container-ffmpeg-codec-matrix.md) — dev-container codec matrix
  rationale (libaom exclusion, SVT-AV1 source build).
- [ffmpeg-patches/README.md](../../ffmpeg-patches/README.md) — patch application
  invariants and verification gate.
- `req` — "of course this has to be fully connected to a ffmpeg worker as well (latest
  of course)..." (architecture popup, 2026-05-28, captured verbatim in ADR-0709
  §References).
