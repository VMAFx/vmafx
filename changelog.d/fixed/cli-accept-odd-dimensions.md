- **`vmaf` CLI now accepts odd dimensions for raw YUV 4:2:0 and 4:2:2 inputs.**
  `validate_chroma_alignment()` (`core/tools/vmaf.cpp`, ADR-0461) previously
  refused odd widths for 4:2:0 and 4:2:2 inputs and odd heights for 4:2:0
  inputs. However, `.y4m` inputs with odd dimensions were already accepted and
  processed with ceiling chroma extent (`vmaf_chroma_extent()`,
  `core/src/picture_geometry.h`, PR #1643). Per user decision 2026-10-01
  ("Accept both (Recommended)"), the CLI accepts odd dimensions for raw YUV
  inputs with ceiling chroma, bit-identically matching `.y4m` scores for identical
  content (ADR-1398). Files with mismatched frame byte sizes continue to exit 2
  cleanly via `yuv_check_file_size()`.
