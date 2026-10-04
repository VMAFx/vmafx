- Every build now stores its GPU device code compressed at the strongest
  setting of its toolchain, through the new Meson option `compress_device_code`
  (default `true`): nvcc compresses every fatbin entry in `size` mode (before,
  only PTX was compressed and the cubins went in raw), hipcc and icpx use
  `--offload-compress` at zstd level 22, and the SYCL SPIR-V image, generated
  by the final link, is compressed there too. The CUDA kernels shrink from
  10.97 MB to 2.60 MB, the HIP kernels for the 25 tester targets from 17.9 MB
  to 1.09 MB, and `libvmaf.so` from 14.2 to 5.9 MB (CUDA), 21.0 to 4.2 MB (HIP)
  and 12.8 to 10.2 MB (SYCL); the device code and every score are unchanged.
  The build fails when a fatbin, code object bundle or SYCL image is stored
  raw, and configure fails when a compiler cannot compress: builds with the
  clang CUDA driver (`enable_nvcc=false`) or AdaptiveCpp need
  `-Dcompress_device_code=false` (ADR-1590).
