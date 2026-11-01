## FFmpeg patch series rebuilt from 0001 on the vmafx filters (2026-11-01)

`feat/2435-patch-series`, [ADR-2166](adr/2166-ffmpeg-series-drops-vulkan-shims.md).
The series is six patches: `0001` qpfile, `0002` autotune CLI glue, `0003`
profile CLI glue, `0004` the `vmafx` filter (`ffmpeg-patches/src/vf_vmafx.c`),
`0005` the `vmafx_pre` filter and `0006` the fork-only loopback-decoder
hwaccel. No patch adds a filter or option with `vmaf` in its name. **On
refresh**: keep `0001` and `0006`, regenerate `0004` and `0005` from
`ffmpeg-patches/src/`, and keep the per-patch numbers that
`docs/development/rebase-sensitive-invariants.md` cites in step.
`scripts/ci/ffmpeg-patches-surface-check.sh` refuses a `vmaf`-named filter or
option; `core/test/test_ffmpeg_vmafx_*_contract.py` guard the filter's
contracts without a device.
