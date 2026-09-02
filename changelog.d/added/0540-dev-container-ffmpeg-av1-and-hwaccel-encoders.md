- **dev-MCP container FFmpeg ships AV1 (SVT-AV1) + VVenC + hardware
  encoders (NVENC, oneVPL/QSV, AMF)
  ([ADR-0540](../docs/adr/0540-dev-container-ffmpeg-av1-and-hwaccel-encoders.md)).**
  `dev/Containerfile` stage 3.5 adds `libvpl-dev` (apt), builds
  SVT-AV1 `v2.1.0` + Fraunhofer VVenC `v1.12.0` + AMD AMF `v1.4.36`
  headers from source, and extends FFmpeg's configure with
  `--enable-libsvtav1 --enable-libvvenc --enable-nvenc
  --enable-cuda-nvcc --enable-libvpl --enable-amf
  --disable-filter=amf_capture`. A build-time encoder probe
  iterates the 14 promised encoders against `ffmpeg -encoders`
  and prints `WARN` lines on any compile-in regression, mirroring
  the ADR-0514 backend-probe pattern. Unblocks `vmaf-tune compare`
  sweeps across the cross-vendor encoder matrix (libx264 /
  libx265 / libsvtav1 / libvvenc / libvpx-vp9 / h264_nvenc /
  hevc_nvenc / av1_nvenc / h264_qsv / hevc_qsv / av1_qsv /
  h264_amf / hevc_amf / av1_amf) on hosts with NVIDIA + Intel +
  AMD GPUs. Runtime availability of hardware encoders still
  requires the matching userspace driver (NVIDIA Container
  Toolkit, Intel media-driver, amdgpu-pro for AMF) — the
  per-encoder `compare.py::probe_encoder_available` two-stage
  probe surfaces missing-runtime cases as row-level skips, not
  whole-sweep aborts. libaom is intentionally NOT enabled: the
  fork's `ffmpeg-patches/0007` references libaom struct fields
  that do not exist in any released libaom; SVT-AV1 covers the
  production AV1 lane. Re-enabling libaom is a follow-up gated
  on a patch-0007 ROI-helper fix.
