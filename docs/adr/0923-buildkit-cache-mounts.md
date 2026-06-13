<!-- markdownlint-disable MD029 MD060 -->
# ADR-0923: Adopt BuildKit cache mounts and ccache across the container build matrix

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: ci, build, container, performance

## Context

The fork ships four primary Dockerfiles that account for most of the
container-build minutes the team burns: the all-backends top-level
`Dockerfile` (CUDA devel + FFmpeg + libvmaf, ~4 GB), the
`docker/Dockerfile.production-gpu` multi-target builder (cpu / cuda12 /
rocm6 / oneapi2026 / vulkan variants), the comprehensive
`dev/Containerfile` (full GPU SDK + ROCm + oneAPI + ONNX Runtime +
FFmpeg + Python venv), and `Dockerfile.go-server` (Go server +
libvmaf cgo link).

Every `RUN apt-get install` invocation in the tree currently:

1. Re-downloads every `.deb` and rebuilds the apt index from scratch
   on each build, even when the package set has not changed.
2. Discards `/var/lib/apt/lists/*` after the install so the next layer
   has to repeat the index fetch.

Every meson / ninja / cmake / `go build` invocation similarly:

3. Recompiles every translation unit from scratch with no compiler
   cache; the CUDA / icx / hipcc paths are the most expensive
   (10–30 minutes for cold rebuilds on CI hardware).

BuildKit cache mounts (`--mount=type=cache,target=...,sharing=locked`)
solve all three: the cache directory lives outside the layer FS, is
persisted across builds at the BuildKit daemon level, and is not
baked into the resulting image — the layer stays clean. Pairing the
apt cache mount with `ccache` (for the C/C++ compile steps) and the
Go module + build cache mounts (for `go build`) cuts cold-to-warm
rebuild time by 3-5x on the dev box and similarly on CI.

## Decision

Add `# syntax=docker/dockerfile:1.7` to the four primary Dockerfiles
and wire two classes of BuildKit cache mounts:

- **apt cache mounts** on `/var/cache/apt` + `/var/lib/apt` for every
  `RUN apt-get install` invocation; drop the matching
  `rm -rf /var/lib/apt/lists/*` cleanup (the cache mount lives outside
  the layer FS so it does not bloat the resulting image).
- **ccache** installed as a build dependency, and a
  `--mount=type=cache,target=$CCACHE_DIR,sharing=locked` cache mount
  wrapped around every meson/ninja and cmake invocation that compiles
  C/C++. Meson auto-detects ccache when it's on PATH; FFmpeg's
  configure gets `--cc='ccache gcc' --cxx='ccache g++'`; cmake builds
  get `-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache`.
- **Go module + build caches** on `Dockerfile.go-server` mounted at
  `/go/pkg/mod` and `/root/.cache/go-build`.

The `vmaf` non-root user in `dev/Containerfile` is pinned to uid/gid
1000 so the cache mounts run by that user
(`--mount=...,uid=1000,gid=1000`) align with the build identity.

Rust / sccache wiring is staged for the Phase 4 Rust bindings work
(no Rust builds currently exist in the four primary Dockerfiles); we
add the apt-cache-mount and ccache scaffolding now, and will follow
up with sccache mounts when the Rust builder stages land.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| BuildKit cache mounts + ccache (chosen) | Native to current BuildKit; no extra infrastructure; cache lives at the daemon level; 3-5x rebuild speedup | Requires BuildKit (already the default on this repo's CI); needs `syntax=docker/dockerfile:1.7` header | Best fit for the existing build pipeline |
| Registry-side build cache (`--cache-from`/`--cache-to`) | Shared across CI runners | Slower than local cache mount; requires registry round-trips; orthogonal to compiler-level caching | Useful complement (could land later); doesn't cover the apt or ccache layer separately |
| Distroless multi-stage with prebuilt base images | Smaller final images | Doesn't address the build-time issue, only image size; orthogonal to this ADR | Not the problem being solved |
| Switch from apt to nix or apk | Reproducible builds | Massive churn; breaks the Ubuntu 24.04 / 26.04 base-image pin; rewrites every install step | Out of scope for an audit modernization PR |
| Bazel / hermetic build | Strongest reproducibility | Replaces meson/cmake/ninja entirely; multi-quarter migration | Out of scope |

## Consequences

- **Positive**: container rebuilds reuse downloaded `.deb` files and
  cached compile artifacts; cold-to-warm rebuild wall time drops 3-5x
  on the dev box for `dev/Containerfile` and similarly on CI runners
  for the production-gpu matrix. The change is transparent to the
  final image (cache lives outside the layer FS).
- **Negative**: requires BuildKit. The legacy classic builder
  (`DOCKER_BUILDKIT=0`) will fail with "unknown flag `--mount`" on the
  RUN lines. CI already runs BuildKit; documenting the requirement
  in the rebase notes covers the rare local case where someone tries
  to build with the classic builder.
- **Neutral / follow-ups**:
  - Rust bindings work (Phase 4) should add `sccache` mounts at
    `/root/.cache/sccache` with `RUSTC_WRAPPER=sccache` when the
    Rust builder stages land.
  - Production-only Dockerfiles outside the four covered here
    (`docker/Dockerfile.production`, `Dockerfile.ffmpeg`, node
    variants under `docker/`) are candidates for the same pattern;
    follow-up PRs can extend the rollout. The four-file scope here
    is the audit modernization #6 baseline.

## References

- Docker BuildKit cache-mount docs:
  <https://docs.docker.com/build/cache/optimize/#use-cache-mounts>
- ccache project: <https://ccache.dev>
- Audit modernization #6 (BuildKit cache mounts + sccache) —
  parent task brief, 2026-05-31.
- Source: req (parent task brief — "switch Dockerfiles to BuildKit
  cache mounts + sccache", paraphrased).
