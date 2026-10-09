## ADM second viewing distance on SYCL (2026-10-09)

- `core/src/feature/sycl/integer_adm_sycl.cpp`: `adm_norm_view_dist_extra`
  (`nvde`) in the option table; `rfactor`, `i_rfactor` and
  `csf_normalization_shift` are indexed by viewing distance;
  `enqueue_adm_reductions()` runs once per distance after the scale's DWT,
  into that distance's block of `d_accum`; `collect_view()` concludes one
  distance. The merge callback and the second distance's names are the
  shared `adm_view_dist.c`. Fork-only, nothing upstream to merge. **On
  sync**: an upstream change to the SYCL ADM driver keeps the per-distance
  loop and the `[view]` index.
- `core/test/test_sycl_adm_parity.c`: the recorded option gap is gone;
  `test_adm_two_views_exact` and `test_adm_merged_registrations_exact` hold
  both distances to the CPU's bits. `core/test/test_adm_view_merge.c` and
  `core/test/test_adm_view_dist_contract.py` list `adm_sycl` among the
  merging descriptors.
