- HIP (AMD ROCm) backend runtime — flips
  `core/src/hip/{common,kernel_template}.c` from the audit-first
  `-ENOSYS` scaffold (ADR-0212) to real HIP runtime calls. New
  builds with `-Denable_hip=true` now hard-link `libamdhip64` and
  expose a working `vmaf_hip_state_init` /
  `vmaf_hip_list_devices` / `vmaf_hip_device_count` plus the full
  per-frame async lifecycle (`hipStreamCreateWithFlags`,
  `hipEventCreateWithFlags`, `hipMallocAsync`, `hipMemsetAsync`,
  `hipStreamWaitEvent`, `hipStreamSynchronize`,
  `hipMemcpy`-based readback). `vmaf_hip_import_state` stays at
  `-ENOSYS` pending the first feature-kernel PR (T7-10c). See
  ADR-0212 §"Status update 2026-05-08".
