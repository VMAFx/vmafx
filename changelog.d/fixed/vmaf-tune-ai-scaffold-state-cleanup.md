- Fixed two `vmaf-tune auto` scaffold leftovers: HDR cells now record
  codec-specific `hdr_args` via `vmaftune.hdr.hdr_codec_args()`, and
  per-content recipe thresholds now drive the confidence decisions
  they are already reported for in metadata.
- Aligned the five `fr_regressor_v2_ensemble_v1_seed{0..4}` registry
  rows with ADR-0321's production flip by marking them `smoke: false`
  against their existing PROMOTE-backed sidecars.
