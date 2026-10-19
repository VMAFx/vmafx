## Netflix/vmaf 3e1385bed: FFmpeg built with MSVC in CI (2026-10-08)

- Upstream adds a Windows row to `.github/workflows/ffmpeg.yml` that builds
  FFmpeg `master` through a Meson port of FFmpeg. The fork's equivalent is the
  `ffmpeg-msvc-work` / `ffmpeg-msvc-gate` jobs of
  `.github/workflows/ffmpeg-integration.yml` (required check
  `FFmpeg Windows MSVC`, ADR-2783), which run
  `ffmpeg-patches/test/build-and-run.sh` with `FFMPEG_TOOLCHAIN=msvc`.
  **On sync**: do not import upstream's `ffmpeg.yml` row; the fork has no
  `ffmpeg.yml`.
- `scripts/ci/upstream-consumer-lib.sh` now holds `uc_ffmpeg_graph` (moved
  from `upstream-ffmpeg-compat.sh`'s `ff_graph`); the smoke script's score
  check and the upstream-consumer check share it.
- `docs/getting-started/building-on-windows.md`: the ARM64 toolset notes stay
  under "Native MSVC on Windows ARM64"; "Threads on MSVC", "Library files of
  an MSVC build" and "FFmpeg with MSVC" follow as sections of their own.
- `ffmpeg-patches/0002` and `0008` call `ff_set_pixel_formats_from_list2()`
  for their `enum AVPixelFormat` lists (cl.exe `C4133` with the untyped
  `ff_set_common_formats_from_list2()`). Keep the typed helper when
  refreshing the series. The smoke script's warning gate under `msvc` reads
  only the lines the series writes and the linker (`msvc_findings`).
