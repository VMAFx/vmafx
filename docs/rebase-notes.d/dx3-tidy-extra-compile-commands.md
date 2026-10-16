## Clang-tidy compilation-database entries for hand-built sources (2026-10-15)

- `scripts/ci/gen-mex-compile-commands.py`: extended to be table-driven, generating
  compile-database entries for `ffmpeg-patches/test/fault_inject_libvmaf.c` and
  `scripts/dev/upstream_parity_harness.c` alongside the MATLAB MEX sources, with
  refusal on missing sources.
- `scripts/ci/gen-gpu-compile-commands.py`: appends an entry for the standalone probe
  `scripts/dev/hip_dispatch_drop_probe.hip` in the HIP lane, mirroring device-kernel flags.
- `.config/lint-exceptions.d/clang-tidy-coverage.toml`: deleted exceptions for the three
  sources; re-reviewed `core/test/test_colorspace.c` and
  `core/test/test_sycl_cuda_serial_upload_lifetime.c` with verified facts and Category D
  (120 days) expiry (2027-02-12).
- **On sync**: fork-only tooling and lint infrastructure; no upstream counterpart.
