- Container builds now reference the post-ADR-0700 source layout:
  `meson setup … core` (was `… libvmaf`), the `dev-mcp` stage's
  source copy lands at `/build/vmaf/core/`, and `cd core` precedes
  the build/install ninja invocations. Affected files:
  `dev/Containerfile`, `docker/Dockerfile.production`,
  `docker/Dockerfile.production-gpu` (5 GPU-variant stages),
  `docker/dev/{alpine-3.20,arch,fedora-40}.Dockerfile`. Public
  install paths (`/usr/local/include/libvmaf/`, `libvmaf.so`,
  `libvmaf-dev` apt package, ffmpeg `--enable-libvmaf*` flags) are
  unchanged because they describe the shipped library / package /
  filter surface, not the source tree.
