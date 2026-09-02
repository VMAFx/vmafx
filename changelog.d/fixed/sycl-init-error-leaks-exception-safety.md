- **SYCL init error-path leaks + exception-safety hardening (RC audit)**:
  Several SYCL error paths leaked USM device memory or let a C++ exception
  escape through the C dispatch frame.
  - `integer_adm_sycl.cpp` / `integer_vif_sycl.cpp` `init_fex_sycl`: the early
    `-ENOMEM` returns (device-buffer null-check, `feature_name_dict` failure)
    skipped `close_fex_sycl(fex)`, leaking every USM buffer already allocated.
    They now run the (NULL-safe) cleanup before returning, matching the sibling
    `vmaf_sycl_graph_register` failure path. The ADM null-check now also covers
    the 11 band / CSF USM buffers (`d_ref_band`, `d_dis_band`, `d_csf_f`) it
    previously omitted — a NULL handed to a SYCL kernel page-faults the device
    instead of failing cleanly with `-ENOMEM`.
  - Both files now capture the return of the `vmaf_sycl_memcpy_h2d` LUT upload
    (division LUT for ADM, log2 LUT for VIF) instead of discarding it; on error
    the host LUT is freed, `close_fex_sycl` runs, and the error is propagated,
    avoiding an uninitialised on-device LUT after an `-EIO` copy.
  - `common.cpp` `vmaf_sycl_graph_submit`: `record_combined_graphs()` (which
    issues throwing SYCL graph-recording APIs) is moved inside the existing
    `try` block so a `sycl::exception` is caught and converted to `-EIO` rather
    than escaping through the C boundary (UB / `std::terminate`).
  - `dmabuf_import.cpp` `vmaf_sycl_import_va_surface_readback`: the VA-surface
    readback `q->memcpy` / `q->wait()` are wrapped in `try`/`catch`; on a
    synchronous `sycl::exception` the mapped VA buffer and `VAImage` are
    released (`vaUnmapBuffer` + `vaDestroyImage`) and `-EIO` is returned,
    preventing both a leak and an exception crossing the C boundary.
