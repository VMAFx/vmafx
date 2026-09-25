- Track shared header dependencies for CUDA fatbin and HIP HSACO device targets
  using explicit Meson `depend_files` and compiler depfiles, ensuring header-only
  changes trigger incremental Ninja rebuilds across all 44 device targets.
  Guarded by device-free contract `test_device_target_header_dependencies`.
