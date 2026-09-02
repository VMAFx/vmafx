### Thread-Safety Audit: CUDA / SYCL / HIP Backends (ADR-0777)

Completed a read-only thread-safety audit of the CUDA, SYCL, and HIP backends
plus the shared `libvmaf.c` / `log.c` / `gpu_dispatch_env.c` infrastructure.
Key findings (no fixes applied):

- `VmafContext *` handles are single-thread-only; no internal lock guards concurrent API calls.
- SYCL `VmafSyclState` frame-counters are mutated without a lock; concurrent callers would race.
- CUDA uses the primary-context `cuCtxPushCurrent/Pop` model with a `_Thread_local` drain stream.
- `vmaf_log_level` / `istty` in `log.c` are plain `int`/`enum`, not `_Atomic`; C11 data race on init.
- `VMAF_BATCH_THREADING` writes `fex->prev_ref` on the shared descriptor from multiple workers.
- No public thread-safety contract exists in `core/include/libvmaf/libvmaf.h`.

Four follow-up items enumerated in ADR-0777.
