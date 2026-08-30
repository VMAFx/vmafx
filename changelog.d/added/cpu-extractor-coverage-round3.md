- Add CPU feature extractor coverage tests (round 3): `test_ssim_coverage.c`,
  `test_adm_coverage.c`, `test_psnr_hvs_coverage.c`, `test_ssimulacra2_coverage.c`;
  18 new test cases covering enable_db/clip_db branches, ADM debug mode,
  psnr_hvs bpc-rejection and chroma-disable paths, ssimulacra2 multi-format
  and HBD pipelines — all wired into `suite=fast`.
