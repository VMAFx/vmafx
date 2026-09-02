- Fixed a pool free-list corruption introduced by PR #838 in CUDA builds: the
  `done=true` early-return branch of `vmaf_read_pictures` called the full
  `read_pictures_cuda_cleanup()`, which unreffed `ref_host`/`dist_host` a second
  time after `threaded_read_pictures_batch` had already released them at line
  1858. The double-unref corrupted the picture pool free-list, causing the next
  `vmaf_picture_pool_fetch` call to deadlock in `pthread_cond_wait` once the
  pool was exhausted. Fix: split `read_pictures_cuda_cleanup` into a full variant
  (host + device) and a `_device_only` variant; the `done=true` path now calls
  only `_device_only`, which releases just the device-side pictures that
  `threaded_read_pictures_batch` did not touch.
- Fixed a coverage-gate breach in `core/src/dnn/ort_backend.c`: commit 674abf299
  added `ort_log_and_release_status()` with an unreachable `else` branch (ORT
  always provides a non-empty message when `st != NULL`). The dead branch
  suppressed coverage below the 83% per-file security floor (ADR-0922). Fix:
  collapse the `if/else` into a single `vmaf_log` call with an inline ternary,
  removing the dead code path entirely.
