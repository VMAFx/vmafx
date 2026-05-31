- **SYCL extractor init-failure cleanup leaks.** Four SYCL feature
  extractors leaked already-allocated USM buffers on init failure
  paths because the early-return after a downstream allocation /
  registration step did not invoke the per-extractor close helper:

  - `integer_adm_sycl.cpp` — `div_lookup` malloc failure,
    `feature_name_dict` failure, and `vmaf_sycl_graph_register`
    failure each returned without calling `close_fex_sycl(fex)`.
  - `integer_vif_sycl.cpp` — `log2 LUT` malloc failure,
    `feature_name_dict` failure, and `vmaf_sycl_graph_register`
    failure each returned without calling `close_fex_sycl(fex)`.
  - `speed_chroma_sycl.cpp` — the post-`ALLOC_*` consolidated
    null-check (any of d_plane / h_plane_ref / h_eigenvalues /
    h_Q / h_R) and the `feature_name_dict` check returned without
    calling `free_sycl_state(s)`.
  - `speed_temporal_sycl.cpp` — identical pattern (six USM checks
    + dict) returned without calling `free_sycl_state_st(s)`.

  Net: every failed init leaked the entire USM working set
  (`d_dwt_tmp_ref/dis`, `d_ref_band[4]`, `d_dis_band[4]`, `d_csf_f[3]`,
  `d_div_lookup`, `d_cm_accum`, `d_csf_den_accum`, `h_cm_accum`,
  `h_csf_den_accum` for ADM; similar working sets for the others).

  All four sites now call the existing close / free helper before
  returning the error. Identified by sycl-reviewer agent audit
  2026-05-30 (HIGH + MEDIUM severity).
