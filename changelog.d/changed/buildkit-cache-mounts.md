- **Container builds**: switch `Dockerfile`, `docker/Dockerfile.production-gpu`,
  `dev/Containerfile`, and `Dockerfile.go-server` to BuildKit syntax 1.7 with
  `--mount=type=cache,sharing=locked` cache mounts on `/var/cache/apt` +
  `/var/lib/apt` for every `apt-get install`. The matching
  `rm -rf /var/lib/apt/lists/*` cleanups are dropped — the cache mount lives
  outside the layer FS and does not bloat the resulting image. Cold-to-warm
  rebuilds skip the network fetch and re-index step (ADR-0923).
- **Container builds**: install `ccache` and wire a `--mount=type=cache,
  target=$CCACHE_DIR` cache mount around every meson/ninja and cmake invocation
  that compiles C/C++ (libvmaf, FFmpeg, SVT-AV1, vvenc, vpl-gpu-rt). FFmpeg's
  configure gets `--cc='ccache gcc' --cxx='ccache g++'`; cmake builds get
  `-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache`. Meson auto-detects ccache on
  PATH. 3-5x cold-to-warm rebuild speedup on the dev box (ADR-0923).
- **Container builds**: pin the `vmaf` non-root user in `dev/Containerfile` to
  `uid=1000 gid=1000` so BuildKit cache mounts run by that user
  (`--mount=...,uid=1000,gid=1000`) resolve to the same identity that runs the
  build (ADR-0923).
- **Container builds**: `Dockerfile.go-server` adds Go module + build cache
  mounts (`/go/pkg/mod` and `/root/.cache/go-build`) so `go mod download` and
  `go build` reuse package archives across builds (ADR-0923).
