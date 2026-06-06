### Changed

- Added `@thread-safety`, `@param`, and `@return` Doxygen tags to all
  previously undocumented public API entry points in
  `core/include/libvmaf/`:
  - `libvmaf_cuda.h` — all 5 functions now carry `@thread-safety`.
  - `libvmaf_sycl.h` — all 20 functions now carry `@thread-safety`
    (per-state ownership model documented; `vmaf_sycl_list_devices` marked
    thread-safe).
  - `dnn.h` — 9 functions now carry `@thread-safety`; pure query functions
    (`vmaf_dnn_available`, `vmaf_dnn_verify_signature`,
    `vmaf_dnn_session_attached_ep`) correctly marked safe from any thread.
  - `picture_v2.h` — all 5 stub functions now carry `@param`, `@return`,
    and `@thread-safety`; `vmaf_backend_handle_name` marked thread-safe.
  - `libvmaf.h` — `VmafPoolingMethod` enum now has a `@brief` block
    explaining each pooling strategy and the `NB` sentinel caveat.
  - `model.h` — `vmaf_model_version_next` now carries `@thread-safety`
    (read-only built-in table, safe from any thread after init).
