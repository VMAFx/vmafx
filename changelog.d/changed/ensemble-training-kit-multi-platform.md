- **ai-tools**: ensemble-training-kit (ADR-0324) extended for
  multi-platform support — NVIDIA CUDA, Intel Arc / iGPU SYCL, Vulkan
  / CPU fallback, and macOS (Apple Silicon + Intel) with VideoToolbox.
  New `_platform_detect.sh` helper auto-defaults the encoder list per
  host: `*_nvenc` on NVIDIA, `*_qsv` on Intel iHD, `*_videotoolbox`
  on Darwin, `libx264` CPU baseline elsewhere. `01-prereqs.sh` skips
  NVIDIA gates on non-CUDA platforms; `02-generate-corpus.sh` and
  `run-full-pipeline.sh` honour the auto-default unless `--encoders`
  is overridden. New `build-libvmaf-binaries.sh` lets each operator
  build a libvmaf binary for their box and rsync it into
  `binaries/<platform>/`; binaries themselves are not in source
  control. `scripts/dev/hw_encoder_corpus.py` now encodes via
  `{h264,hevc}_videotoolbox` using the canonical
  `_videotoolbox_common.py` adapter's argv shape. New
  `tests/test_platform_detect.sh` covers the eight detection
  branches. Per-box corpus shards merge via
  `ai/scripts/merge_corpora.py` for the cross-platform LOSO retrain.
