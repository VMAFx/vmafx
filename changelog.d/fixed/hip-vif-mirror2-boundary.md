### Fixed

- **integer_vif_hip parity gap closed (places=6)**: All filter-loop boundary
  reads in `vif_statistics.hip` used `clamp_i` (replicate-edge), disagreeing
  with the CPU reference's symmetric reflect (`PADDING_SQ_DATA`) and the CUDA
  twin's two-bounce mirror. This produced max |HIP−CPU| ≈ 0.0018 per VIF scale
  (places~2.75) on the Netflix src01 576×324 pair, violating the ADR-0214
  places=4 gate. Replacing `clamp_i` with `mirror2_i` brings all four VIF
  scales to places~6 (max delta ≈ 1e-6) across 48 frames on gfx1030 wave32
  hardware. The in-repo parity test tolerance is tightened from 1e-3 to 1e-4.
  (ADR-1103)
