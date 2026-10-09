## ADM second viewing distance on HIP (2026-10-09)

- `core/src/feature/hip/integer_adm_hip.c`: `adm_norm_view_dist_extra`
  (`nvde`) in the option table; `adm_hip_scale0()` / `adm_hip_scale123()`
  became `*_transform()` (DWT) and `*_weigh()` (denominator, CSF, CM, AIM at
  one distance, through `adm_hip_view_buffer()`); `rfactor` / `i_rfactor` are
  indexed by distance and `adm_hip_fixed_params()` takes the distance;
  `tmp_res` and `results_host` hold one result block per distance; the
  context initialisers and the host conclusion take the distance as `nvd`.
  The merge callback and the second distance's names are the shared
  `adm_view_dist.c`. Fork-only, nothing upstream to merge. **On sync**: an
  upstream change to the HIP ADM driver lands in the matching half; keep the
  per-distance loop.
- `core/test/test_hip_adm_exact.c`: `test_adm_two_views_exact` and
  `test_adm_merged_registrations_exact` hold both distances to the CPU's
  bits. `test_hip_adm_parity.c` drops its recorded option gap;
  `test_hip_adm_exact_contract.py` and
  `test_hip_adm_buffer_pointer_contract.py` pin the `nvd` contexts, the
  `res_bytes` clear and `adm_hip_init_names()`'s failure path.
