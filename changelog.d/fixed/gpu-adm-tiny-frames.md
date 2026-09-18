- **The CUDA and HIP `integer_adm` twins now score frames 17 to 32 pixels wide
  correctly, and every GPU twin refuses frames below 17x17.** At those widths
  one of scale 0's right shifts is by zero bits. The CUDA and HIP host code
  set its rounding term to 2^31 instead of 0, and their scale-0 kernels read
  one column and one row past the band at the right and bottom edges. Scale 0
  came out up to 0.2 away from the CPU, and 32x32 frames scored NaN. The CUDA,
  HIP and SYCL twins also accepted frames smaller than 17x17, which the CPU
  and Metal extractors refuse; they now fail with `-EINVAL` and an error that
  names the extractor. Scores for frames wider than 32 pixels do not change.
