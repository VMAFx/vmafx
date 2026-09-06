- `docs/metrics/ms-ssim.md` now states the per-backend reality of the `enable_chroma`
  option instead of claiming GPU twins "expose only `enable_lcs`". Read from the twins'
  own option tables: SYCL implements chroma fully, HIP accepts the option as a documented
  no-op with the chroma features falling back to the CPU twin by name, and CUDA does not
  accept it and fails loudly with `unknown option 'enable_chroma'`. Also records that
  `--feature float_ms_ssim=enable_chroma=true` never reaches a GPU twin, because
  `--feature` selects the CPU extractor. Tracked as
  `T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06`.
