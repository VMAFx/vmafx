---
paths:
  - core/src/feature/ssimulacra2_pixel_format.h
  - core/src/feature/ssimulacra2.c
  - core/src/feature/cuda/ssimulacra2_cuda.c
  - core/src/feature/sycl/ssimulacra2_sycl.cpp
  - core/src/feature/hip/ssimulacra2_hip.c
  - core/src/feature/metal/ssimulacra2_metal.mm
  - core/test/test_ssimulacra2_pixel_format.c
  - core/test/test_ssimulacra2_pixel_format_contract.py
invariant: Every ssimulacra2 init() refuses 4:0:0 first, through vmaf_ss2_check_pixel_format(); no private check.
---
# SSIMULACRA 2 input pixel formats

- colour conversion of every ssimulacra2 extractor reads U and V
  planes. 4:0:0 picture has neither (`picture.c` leaves `data[1]` /
  `data[2]` NULL and their sizes 0), and chroma reader then clamps its
  coordinate to -1 and reads before NULL. CPU extractor crashed that way
  until `T-SSIMULACRA2-CPU-YUV400-NULL-CHROMA-2026-10-01`, Metal twin
  until `T-METAL-SSIMULACRA2-YUV400-ACCEPTED-2026-10-05`.
- `ssimulacra2_pixel_format.h` is one check. `init()` of CPU
  extractor and of CUDA, SYCL, HIP and Metal twins calls
  `vmaf_ss2_check_pixel_format(pix_fmt, "<extractor name>")` before it
  allocates anything; ADR-1324 context checks use `vmaf_ss2_has_chroma()`.
  A new twin does same. Do not add `VMAF_PIX_FMT_YUV400P` comparison of
  your own and do not discard `pix_fmt` in `init()`:
  `core/test/test_ssimulacra2_pixel_format_contract.py` fails on either.
- header is host code only (it calls `vmaf_log()`). Keep it out of
  `ssimulacra2_score.h`, which CUDA and HIP kernels compile as device
  code.
- CPU's message `ssimulacra2: needs a YUV 4:2:0, 4:2:2 or 4:4:4 input,
  not 4:0:0` is quoted in `docs/metrics/ssimulacra2.md`;
  `test_ssimulacra2_pixel_format` pins it.
