- **fix(cuda)**: add `static` to 12 TU-internal helper functions across
  `integer_vif_cuda.c`, `integer_adm_cuda.c`, and `integer_motion_cuda.c` —
  eliminates ODR name-clash risk in static-lib builds; restores parity with
  the HIP twin (`integer_adm_hip.c`) which already marks these `static`
  (ADR-1011).
