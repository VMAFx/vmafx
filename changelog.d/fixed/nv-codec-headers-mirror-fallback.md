- The dev container no longer becomes unbuildable when `code.ffmpeg.org` is down. The
  nv-codec-headers layer now falls back to `github.com/FFmpeg/nv-codec-headers` for the same
  pinned tag, and asserts the archive actually carries `ffnvcodec/dynlink_cuda.h` and the
  `cuStreamCreateWithPriority` declaration before installing it, so a fallback cannot
  silently install the wrong headers. That host was unreachable for over six hours on
  2026-09-06, stalling the layer with no symptom beyond an apparent hang — and under
  CLAUDE.md rule 15 and ADR-1102 the container is the canonical build environment. See
  [ADR-1200](docs/adr/1200-nv-codec-headers-mirror-fallback.md).
