- **`scripts/dev/hw_encoder_corpus.py`** — Phase A real-corpus runner.
  Encodes a raw YUV with NVENC / QSV / VAAPI / libx264 at a CRF/CQ
  grid, decodes back to raw YUV, scores with libvmaf (CUDA backend),
  and emits one JSONL row per (source, encoder, cq, frame) carrying
  canonical-6 features (`integer_adm2`, `integer_vif_scale0..3`,
  `integer_motion2`) + per-frame VMAF + encode metadata. This is the
  per-frame schema [`fr_regressor_v2`](../../ai/scripts/train_fr_regressor_v2.py)
  needs for real (non-smoke) training; the existing
  `vmaf-tune corpus` CLI emits only pooled VMAF and was a Phase A
  scope-cut. Smoke evidence: a local 9 sources × 6 hardware codecs
  (h264 / hevc / av1 on NVIDIA NVENC + Intel Arc QSV) × 4 CQ values
  produced **33,840 per-frame rows** in ~5 minutes wall time on an
  RTX 4090 + Arc A380 host — both engines run in parallel because the
  encode hardware is on different cards. Full output lands in
  `runs/phase_a/` (gitignored); rerun the script to reproduce. New
  fork-internal doc `docs/development/intel-arc-vaapi-driver-priority.md`
  captures the `LIBVA_DRIVER_NAME=iHD` gotcha for multi-card hosts.
