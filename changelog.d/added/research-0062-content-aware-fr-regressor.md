- **Research-0062: content-aware fr_regressor_v2 feasibility.**
  Tested whether adding a 6-dim content-class one-hot
  (animation / sports / film_drama / wildlife / ugc / unknown) to
  the `fr_regressor_v2` codec_block lifts PLCC on the 216-row
  Phase A real corpus. Result: PLCC flat (Δ +0.001), RMSE regressed
  by 0.46 VMAF units. The corpus is too sparse for the added one-hot
  capacity (9 rows per genre×codec×cq cell). Content awareness parked
  until corpus size 10x's, LOSO surfaces a per-genre gap, or
  auto-extracted continuous content features replace the manual
  genre tag.
