- **docs(getting-started)**: Backfill Intel QSV install matrix across
  the six per-OS install pages (Arch / Fedora / Ubuntu / macOS /
  Windows / Alpine — Alpine inherits the CPU-only carve-out, macOS
  is documented as unsupported). Each page now lists the verified
  oneVPL / `libvpl` package names for the distro, the FFmpeg build
  flag (`--enable-libvpl` from FFmpeg n6.0+, legacy
  `--enable-libmfx` otherwise), and the Intel CPU / GPU generation
  matrix mapping Skylake → Battlemage to H.264 / HEVC 8-bit / HEVC
  10-bit / AV1-decode / AV1-encode capability. Closes the
  discoverability gap surfaced by the SYCL audit (research-0086,
  Topic C / issue #464) for the three QSV codec adapters
  (`h264_qsv`, `hevc_qsv`, `av1_qsv`) shipped under
  [ADR-0281](docs/adr/0281-vmaf-tune-qsv-adapters.md). No adapter
  code or runtime probe changed; the runtime probe in
  `_qsv_common.ffmpeg_supports_encoder` already covers both
  `libmfx` and `libvpl` correctly.
