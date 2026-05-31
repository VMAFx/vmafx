**test(cuda)**: Fix two bugs in `test_gpu_picture_pool.c` (Round 27 audit D.3 + D.4):

- **D.3** — Remove `malloc(sizeof(VmafCudaState))` from the `VmafCudaCookie`
  initialiser in `test_ring_buffer`. `vmaf_cuda_state_init` allocates
  internally through a `VmafCudaState **` output parameter, so the pre-allocated
  block was unconditionally leaked on every run. ASan reports this as a definite
  leak on CUDA-capable machines.
- **D.4** — Delete the dead `/* ... */` block containing `test_ring_buffer_threaded`
  and its helper `request_picture`. The block contained two latent compilation
  errors (duplicate `cfg` declaration; `VmafCudaState *` passed where
  `VmafCudaState **` is required) and was never active — commented out since
  the file was first created in PR #266 (ADR-0239). No coverage is lost.

See [ADR-0970](docs/adr/0970-test-gpu-picture-pool-cleanup.md).
