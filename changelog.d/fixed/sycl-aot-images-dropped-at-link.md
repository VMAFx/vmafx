- SYCL builds now contain the native Intel GPU code that `sycl_icpx_aot_targets`
  asks for. Since ADR-0568 the images were compiled into the objects and then
  dropped at the link, so every `libvmaf.so` was SPIR-V only and compiled its
  kernels on each cold start: the default model's first frame took 524 ms on an
  Arc B580 with a cold compiler cache and now takes 201 ms (UHD 770: 609 to
  245 ms); scores are bit-identical. SYCL sources are compiled with
  `-fno-sycl-rdc --offload-compress`, so the images are built at compile time
  and survive every link; `libvmaf.so` grows from 4.6 to 7.7 MB. An AOT build
  now needs Intel's `ocloc` on `PATH` (`scripts/ci/install-intel-ocloc.sh`
  installs the release pinned as `INTEL_NEO_VERSION` in `build-config.env`),
  and configure stops with an explanation without it; configure with
  `-Dsycl_icpx_aot_targets=` for a SPIR-V-only build. On Linux the build fails
  if `libvmaf.so` lacks an image for any requested target (ADR-1360).
