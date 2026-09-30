- **SYCL: kernels stay out of scratch memory, and a wrong-result driver is
  reported.** On an Arc A380 under the Linux xe driver, a SYCL kernel that keeps
  a private array in memory or spills registers returns wrong values, with no
  error; 25 of the 109 SYCL kernels did. `vif_sycl`'s SIMD-32 kernels now use the
  256-entry register file and no longer spill: forced with the new
  `VMAF_SYCL_VIF_SUBGROUP_SIZE=32`, they scored every frame 0/0 on that device
  and now match the SIMD-16 kernels bit for bit. The first SYCL initialisation on
  each device runs two probe kernels and logs a warning naming the SYCL
  extractors that still use scratch memory when the probes come back wrong
  (`VMAF_SYCL_SCRATCH_SELFTEST=0` skips it); the device is used either way. The
  new `test_sycl_kernel_scratch` fails when a kernel outside
  `core/src/sycl/scratch_ratchet.txt` uses scratch memory. See
  [Scratch memory on Intel GPUs](docs/backends/sycl/overview.md#scratch-memory-on-intel-gpus-adr-1395)
  (ADR-1395, `T-SYCL-XE-SCRATCH-WRONG-RESULTS-2026-10-01`).
