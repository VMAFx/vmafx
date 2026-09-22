- **The SYCL `integer_adm` twin now scores full-range content like the CPU.**
  The CPU keeps its scale-0 intermediate values in 16 bits, so very large
  values wrap around there, and the SYCL twin kept them in 32 or 64 bits.
  On noisy content, such as two frames of independent random noise,
  `integer_adm_scale0` came out 2.1e-4 away from the CPU, over the 1e-4
  cross-backend tolerance, and up to 4.4e-3 away with larger CSF weights.
  The SYCL twin now wraps at the same points. It also rounds two scale 1-3
  terms the way the CPU, CUDA and HIP do (the long-standing Netflix#955
  quirk). That moves SYCL's scale 1-3 and `integer_adm2` scores on ordinary
  content by less than 1e-6, toward the CPU. The remaining differences are
  below 3e-7.
