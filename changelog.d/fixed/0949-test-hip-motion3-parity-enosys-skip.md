- `test_hip_motion3_parity` skips cleanly when libvmaf is built with
  `enable_hip=true` but `enable_hipcc=false` — the HIP `motion_hip`
  extractor's submit returns `-ENOSYS` in that posture (HSACO device
  kernels are not embedded) and the test now matches that signal to
  the same skip path as the no-HIP-device branch, emitting
  `[skip: HIP kernels not built (enable_hipcc=false)]`. Fixes the
  pre-existing `HIP: vmaf_read_pictures failed` failure flagged by
  the PR #443 audit. Real parity failures still surface loudly when
  the kernels are compiled. See ADR-0949.
