- **The dev container could not build.** `dev/Containerfile` pinned
  `IGC_VER=2.40.13+21428`, but `+21428` is the build-id of IGC **v2.34.4** — the
  v2.40.13 assets are `+22418`. Both `intel-igc-core-2_…` and
  `intel-igc-opencl-2_…` downloads returned HTTP 404, so every image build failed
  at that layer. Now `2.40.13+22418`, verified HTTP 200.
- Intel GPU stack bumped to current and **co-versioned**: compute-runtime (NEO)
  `26.18.38308.1` → `26.31.39395.13`, vpl-gpu-rt `intel-onevpl-26.1.5` →
  `26.2.4`. `GMMLIB_VER` deliberately stays at `22.10.0`: the Containerfile pulls
  `libigdgmm12` *out of the compute-runtime release*, and NEO 26.31 bundles
  22.10.0 — bumping to the standalone gmmlib 22.10.1 would 404. All five
  resulting download URLs verified HTTP 200.
- `intel/oneapi-basekit:2026.0` (referenced by `docker/Dockerfile.node` and
  `docker/Dockerfile.production-gpu`) **does not exist** — the newest published
  tag is `2025.3.2-0-devel-ubuntu24.04`. Same for the
  `intel-oneapi-dpcpp-cpp-2026.0` apt package. Both corrected.
- `docker/Dockerfile.production-gpu` was on **CUDA 12.0** while the rest of the
  repo is on 13.3.1; `docker/Dockerfile.node` was a release behind on SVT-AV1
  (4.1.0 vs 4.2.0). nv-codec-headers moves from an 11-month-old pinned SHA to the
  `n13.1.15.0` tag.
- **Renovate was silently not managing several of these.** Its `pre-commit`
  manager ships `enabled: false` by default and the config never turned it on, so
  the `packageRules` entry targeting `matchManagers: ["pre-commit"]` never fired —
  which is why 5 of 10 hooks were stale (one by three majors) while every manager
  Renovate *does* run was at latest. The `kubernetes` manager has no default file
  patterns, so Helm templates were invisible. Every custom manager was scoped to
  `dev/Containerfile` alone, which is why `docker/Dockerfile.node` kept a
  never-existent FFmpeg tag and an outdated SVT-AV1. The gmmlib manager used
  `github-releases` against a project that publishes only tags, and `NEO_VER`'s
  four-component version needs `versioning: loose` to compare at all. All fixed.
