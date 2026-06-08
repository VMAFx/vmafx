- `local_explainer_test`: recalibrate `test_run_vmaf_runner_local_explainer_with_bootstrap_model`
  assertion to the post-NEON-fix value (`75.40974...`) and relax to `places=3`
  per ADR-0418 macOS-libm pattern; fixes macOS arm64 CI failure introduced by PR #834
  NEON uint64-truncation fix (T-LOCAL-EXPLAINER-BOOTSTRAP-NEON-RECAL-2026-06-08).
- `Dockerfile`: add `RUN ldconfig` after `make install` so the installed vmaf binary can
  resolve `libvmaf.so.3` at runtime on the NVIDIA CUDA Ubuntu 24.04 base image, which
  omits `/usr/local/lib/x86_64-linux-gnu` from its dynamic-linker cache
  (T-DOCKERFILE-LDCONFIG-MISSING-2026-06-08).
