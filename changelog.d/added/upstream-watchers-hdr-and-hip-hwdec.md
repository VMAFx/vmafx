- Two more `upstream-*-watcher.yml` workflows per ADR-0448, closing
  the remaining external-trigger deferral rows in `docs/state.md`:
  - `upstream-netflix-645-hdr-model-watcher.yml` — polls
    `Netflix/vmaf:model/` for any `vmaf_hdr_*.json` file weekly +
    tracks Netflix/vmaf#645 activity; opens a fork-side tracking
    issue when an upstream HDR model lands (closes
    HDR-VMAF-MODEL-PORT path A).
  - `upstream-ffmpeg-hip-hwdec-watcher.yml` — uses GitHub code-search
    against FFmpeg/FFmpeg for `hwcontext_hip.h` and the
    `AV_HWDEVICE_TYPE_HIP` enum variant; opens a fork-side issue when
    upstream lands HIP hwdec support (closes T-FFMPEG-HIP-FILTER-DEFERRED).
