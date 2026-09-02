## Added

- **`core/test/test_pixel_format_edge_coverage.c`** — five focused
  end-to-end CPU extractor smoke tests for the
  pixel-format / bit-depth combinations that were previously
  unexercised at the libvmaf unit-test layer:

  | test | extractor | pix_fmt | bpc |
  | --- | --- | --- | --- |
  | `psnr_yuv422p_8bit_identical` | `psnr` | 4:2:2 | 8 |
  | `psnr_yuv444p_10bit_identical` | `psnr` | 4:4:4 | 10 |
  | `psnr_yuv420p_12bit_identical` | `psnr` | 4:2:0 | 12 |
  | `ssim_yuv422p_8bit_identical` | `ssim` | 4:2:2 | 8 |
  | `ciede_yuv422p_8bit_identical` | `ciede` | 4:2:2 | 8 |

  Closes the audit gap surfaced by Research-0912: prior to this PR,
  no CPU extractor was exercised end-to-end on 4:2:2 input, no
  extractor was exercised at 12 bpc, and the only 4:4:4 + HBD smoke
  ran through the full VMAF model path
  (`test_pic_preallocation::test_picture_pool_yuv444`) rather than
  isolating a single extractor.

  The ciede 4:2:2 case in particular exercises the chroma-upscale
  scratch-allocation path
  (`ciede.c::init` → `vmaf_picture_alloc(YUV444P)` × 2,
  `extract` → `scale_chroma_planes`) that the 4:4:4 fast-path bypasses
  entirely. The ssim 4:2:2 case exercises the asymmetric
  `ss_hor=1, ss_ver=0` chroma geometry that does not appear in any
  other end-to-end test. ADR-0912.
