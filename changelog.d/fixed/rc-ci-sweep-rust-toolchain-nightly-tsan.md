- **CI green-up for RC.** Fixed two non-required CI lanes that were red and
  one Docker build break: (1) `rust-ci.yml` set `dtolnay/rust-toolchain` by
  commit SHA, so the action could not infer the toolchain from its git ref —
  added an explicit `toolchain: stable` (was failing every run with
  `'toolchain' is a required input`); (2) the `nightly` ThreadSanitizer lane
  lacked `TSAN_OPTIONS=allocator_may_return_null=1`, so the intentional
  ~192 GB-allocation UAF test hard-aborted — env added (mirrors the required
  tests-and-quality-gates lane); (3) `docker/Dockerfile.production-gpu` did
  `meson setup /build libvmaf` — fixed to `core` (the ADR-0700 build-root
  rename was missed; every GPU production image failed at meson setup). The
  CUDA CI legs stay pinned to 13.2.0: the Jimver CI installer does not publish
  13.3.0 yet (T-CI-JIMVER-CUDA-133), while the Docker/dev images use 13.3.0 —
  an intentional, documented split.
