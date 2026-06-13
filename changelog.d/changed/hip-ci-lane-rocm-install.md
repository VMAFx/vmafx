- **HIP CI lane installs ROCm runtime + headers (T7-10b
  bring-up, ADR-0212 status update)** — the
  `Build — Ubuntu HIP` matrix row in
  [`libvmaf-build-matrix.yml`](.github/workflows/libvmaf-build-matrix.yml)
  now apt-installs `rocm-hip-runtime-dev` from the official AMD repo
  at `https://repo.radeon.com/rocm/apt/7.2.3` so the build can link
  against `amdhip64` for the T7-10b HIP runtime promotion (#499).
  Lane renamed `T7-10 scaffold` → `T7-10b runtime` and added to the
  required-aggregator allow-list — branch-protection now blocks
  merges on a HIP build break. Smoke test
  (`test_hip_smoke`) runs on the runner without an AMD GPU
  (`vmaf_hip_device_count() == 0` short-circuits the device-resident
  assertions per ADR-0212). Cost: ~200 MB apt download + 3-5 min
  wall-clock per HIP-lane run; acceptable because HIP is opt-in via
  `-Denable_hip=true` and only this row pays it.
