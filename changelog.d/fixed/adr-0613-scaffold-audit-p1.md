# Fixed — ADR-0613: scaffold-audit P1 feature-plumbing fixes

- **vmaf-tune**: `compare` and `tune-per-shot` now call `select_backend()` before any
  bisect or CRF-sweep work.  An explicit `--score-backend cuda` on a CPU-only binary
  exits 2 with a clear `BackendUnavailableError` message instead of crashing mid-bisect
  with an opaque vmaf binary error.  Mirrors the pre-check already present in `ladder`
  (ADR-0511) and `corpus` (ADR-0299/ADR-0314).
- **HIP**: `vmaf_hip_picture_alloc` now calls `hipMalloc` to allocate device memory and
  `vmaf_hip_picture_free` calls `hipFree` to release it.  The previous stubs returned
  `-ENOSYS`, silently blocking picture upload for all 9 HIP extractors.
- **mobilesal extractor**: `bpc != 8` rejection at `init()` now prints an actionable error
  message naming `feature_mobilesal` as the blocker and gives the workaround (`--bitdepth 8`
  or omit `--feature mobilesal` for HDR content).  `docs/ai/models/mobilesal.md`
  §Known limitations updated with example output and workaround.
- **DNN API**: Both `-ENOTSUP` sites in the attached DNN path (`vmaf_ctx_dnn_run_frame_nchw`
  and `vmaf_ctx_dnn_run_frame_feature_vector`) now carry inline comments explaining the
  single-output constraint.  `docs/api/dnn.md` §Known limitations documents the limitation,
  the standalone `vmaf_dnn_session_run()` workaround, and the T-DNN-MULTI-OUTPUT tracking
  entry in `docs/state.md`.
