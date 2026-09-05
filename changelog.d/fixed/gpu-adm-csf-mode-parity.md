- `integer_adm` GPU twins now honour the CPU option table instead of
  silently dropping half of it. `adm_csf_mode` (all four CSF models:
  Watson97, Barten, Barten/Watson blend, Barten/Watson blend MAE) and
  `adm_p_norm` are implemented on CUDA, SYCL and HIP; each twin's
  `VmafOption` table is now an entry-for-entry mirror of
  `core/src/feature/integer_adm.c` (name, alias, type, default,
  bounds, `VMAF_OPT_FLAG_FEATURE_PARAM`), so the emitted feature-name
  key matches the CPU twin's byte-for-byte. Before this change the
  default model's ADM request
  (`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`) resolved on a
  GPU backend to an extractor that ignored `adm_csf_mode` and emitted
  a differently-named key.
- `adm_min_val` no longer clamps `VMAF_integer_feature_adm2_score` on
  CUDA, SYCL or HIP. The CPU reference applies the floor to the adm3
  expression only — the Netflix golden `adm_min_val=0.98` case pins
  adm2 at `0.9345148541666667`, below the floor.
- The ADM `numden_limit` precision floor on all three GPU twins now
  scales with the full-frame area, matching
  `integer_adm.c::integer_compute_adm`. The twins were using the
  scale-3 dimensions left in the loop variables, a floor 256× too
  small.
- The SYCL and HIP `integer_adm` twins no longer emit
  `VMAF_integer_feature_aim_score` / `VMAF_integer_feature_adm3_score`
  from a hard-coded `aim_num = 0.0`. Neither backend has the AIM
  device pass (the CUDA twin's ADR-0746 kernels); both features are
  now left out of `provided_features[]` so the ADR-0530 name lookup
  routes them to the CPU twin, which returns the correct value under
  the correct key.
