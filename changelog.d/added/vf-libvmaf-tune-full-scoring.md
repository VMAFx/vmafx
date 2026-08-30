- **`vf_libvmaf_tune` filter full scoring (ADR-0312 sub-decision retired).**
  `ffmpeg-patches/0008-add-libvmaf_tune-filter.patch` graduates from the
  scaffold pass-through state to a real in-process VMAF scorer: per-frame
  `vmaf_read_pictures(ref, dist, idx)` mirroring `vf_libvmaf.c`'s CPU
  framesync pipeline, with `vmaf_score_pooled(VMAF_POOL_METHOD_MEAN)` at
  uninit. The `recommended_crf=…` log line now reports a real
  `observed_vmaf` alongside `target_vmaf` and `n_frames`; the CRF
  recommendation is still a piece-wise linear projection of the observed
  VMAF onto `[recommend_crf_min, recommend_crf_max]` (per-clip Optuna TPE
  search stays in `tools/vmaf-tune/src/vmaftune/recommend.py`). Smoke:
  `ffmpeg -hide_banner -f lavfi -i "color=red:size=128x128:r=10:d=1" -f
  lavfi -i "color=red:size=128x128:r=10:d=1" -lavfi "[0:v][1:v]libvmaf_tune=recommend_target_vmaf=95"
  -f null -` reports `observed_vmaf=97.43` (real pooled score, not the
  scaffold's static 95.0 placeholder).
