- **The CUDA parity-gate default run skips on a build without CUDA.**
  `test_cuda_parity_gate_default_run` is registered for every build, and on a
  libvmaf built without CUDA the `vmaf` CLI refuses `--backend cuda`, which
  the test reported as a failed parity gate: `--suite=gpu` on a HIP-only or
  SYCL-only build had one failing test. The refusal is now a skip, like a
  missing device, and `test_cuda_parity_gate_skip` pins the decision to the
  CLI's message without a device.
