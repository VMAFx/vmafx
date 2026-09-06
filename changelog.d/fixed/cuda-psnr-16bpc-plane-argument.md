- **CUDA chroma PSNR above 8 bpc was a luma number.** `psnr_cuda_dispatch` has
  always passed the plane index to both kernels, but the 16-bpc kernel had no
  parameter for it and read `data[0]` / `stride[0]`, so every high-bit-depth
  chroma dispatch measured a chroma-sized window of the luma plane: on the
  Netflix 576x324 pair at 10 bpc CUDA reported `psnr_cb = psnr_cr = 34.359`
  where the CPU reports 39.255 / 41.375. Luma, 8-bit input, SYCL and HIP were
  all correct. Every PSNR fixture was 8-bit with flat chroma, which is how it
  survived. Fixed by adding the parameter; the parity fixture is now bit-depth
  generic with non-flat, ref/dist-different chroma above 8 bpc and registered
  again at 10 bpc.
