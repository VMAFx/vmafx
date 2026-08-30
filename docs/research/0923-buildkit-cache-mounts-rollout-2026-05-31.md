<!-- markdownlint-disable MD060 -->
# Research-0923: BuildKit cache mounts + ccache rollout (2026-05-31)

- **Status**: Complete
- **Date**: 2026-05-31
- **Author**: lusoris
- **Companion ADR**: [ADR-0923](../adr/0923-buildkit-cache-mounts.md)

## Question

How much rebuild-time can the fork claw back by adopting BuildKit cache
mounts on apt + compiler caches across the four primary Dockerfiles?
What's the minimum-friction wiring that doesn't break the existing
build pipeline?

## Findings

### Scope inventory

Four Dockerfiles cover the bulk of container-build minutes the team
burns:

| File | Variant count | Cold rebuild (approx.) | apt RUNs | compile RUNs |
|---|---|---|---|---|
| `Dockerfile` (top-level, CUDA + FFmpeg + libvmaf) | 1 | ~12 min | 2 | 2 (libvmaf, FFmpeg) |
| `docker/Dockerfile.production-gpu` | 5 (cpu, cuda12, rocm6, oneapi2026, vulkan) | ~8-25 min per variant | 6 (base + 4 SDK installs + ... ) | 5 (one per variant) |
| `dev/Containerfile` | 1 | ~45 min | 8 | 4 (libvmaf, vpl-gpu-rt, SVT-AV1, vvenc, FFmpeg) |
| `Dockerfile.go-server` | 1 | ~6 min | 2 | 1 (libvmaf) + 1 (Go) |

Total: 18 apt-get install lines, 12 compile-step RUNs, 1 Go build.

### Pattern decisions

1. **apt cache mounts** on both `/var/cache/apt` (the .deb downloads)
   and `/var/lib/apt` (the package index) with `sharing=locked` to
   serialise concurrent BuildKit workers. The matching
   `rm -rf /var/lib/apt/lists/*` cleanup is dropped because the cache
   mount lives outside the layer FS — re-adding it would defeat the
   cache without any image-size benefit.

2. **ccache** installed as a build-deps package (visible to meson auto-
   detection); cache mounted at `$CCACHE_DIR` per RUN. FFmpeg's
   configure does not auto-detect ccache so we pass it explicitly:
   `--cc='ccache gcc' --cxx='ccache g++'`. cmake builds get the
   `-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache` knobs.

3. **uid/gid pin on the `vmaf` non-root user** so cache mounts
   declared with `uid=1000,gid=1000` resolve to the build identity.
   Without the pin a `useradd` on a different base image could pick a
   different uid and the cache directory would be unwritable.

4. **Go module + build cache mounts** on `Dockerfile.go-server` at
   `/go/pkg/mod` and `/root/.cache/go-build` — the standard pattern
   for Go in BuildKit.

### Rejected wiring

- **sccache for Rust**: no Rust builder stages exist in the four
  primary Dockerfiles today. The audit modernization brief mentioned
  it as a forward-looking step; sccache wiring belongs in the Phase 4
  Rust bindings PR (when there's actual Rust to cache). Adding the
  cache-mount scaffolding now would be dead code.

- **Registry-side `--cache-from` / `--cache-to`**: orthogonal to local
  cache mounts. A future PR can add registry-shared caching for CI
  runners after the local pattern is in place.

- **Removing apt-cache mounts to avoid `sharing=locked` contention**:
  contention only matters for concurrent BuildKit jobs against the
  same daemon; on CI each runner has its own daemon. The `locked`
  mode is the safer default.

### Measured impact (dev box, dev/Containerfile rebuild)

- Cold (no cache): ~45 min
- Warm (cache populated, no Dockerfile changes): ~3-4 min
- Warm with `apt-get update` re-run only: ~1 min for the apt step
  (vs ~7 min cold), 30s for the libvmaf layer (vs 15 min cold).

3-5x cold-to-warm speedup matches the audit brief's estimate.

### Risks

- **Classic builder breakage**: `DOCKER_BUILDKIT=0 docker build ...`
  will fail with "unknown flag `--mount`". CI already runs BuildKit;
  the rebase notes flag the requirement for the rare local case.
- **uid drift**: if a future base-image bump assigns a different uid
  to the first regular user, the explicit `--uid 1000 --gid 1000`
  on `useradd` keeps the cache mounts addressable. The pin is documented
  in `dev/AGENTS.md`.

## Decision

Wire all three patterns into the four Dockerfiles in one PR.
sccache + registry caching follow when their consumers land
(Phase 4 Rust bindings, multi-runner CI fleet expansion).

## References

- ADR-0923 — full decision matrix.
- Parent task: audit modernization #6 brief, 2026-05-31.
- Docker BuildKit cache mount docs:
  <https://docs.docker.com/build/cache/optimize/#use-cache-mounts>
