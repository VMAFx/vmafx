- ORT error messages are now surfaced via `vmaf_log(WARNING)` instead of
  being silently discarded. All `OrtStatus*` failure paths in
  `core/src/dnn/ort_backend.c` now call `api->GetErrorMessage()` before
  `ReleaseStatus()`, so model load failures, inference errors, tensor shape
  mismatches, and EP initialisation failures produce a human-readable log
  line rather than a bare `-EIO`. EP-availability probes (expected-miss on
  CPU-only ORT builds) emit at `DEBUG` level to avoid noise.
  The `GetTensorElementType` silent-discard regression (accidentally dropped
  in commit `35907a087` when `ort_backend.c` was re-added from a stale state)
  is re-applied: failure now returns `-EINVAL` with a `WARNING` log instead
  of leaving `input_elem_types[i]` / `output_elem_types[i]` at `UNDEFINED`.
