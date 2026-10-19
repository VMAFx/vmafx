- **The dev and SYCL + FFmpeg containers build the CPU code with GCC.**
  `dev/Containerfile` and `Containerfile.vmafx` configure libvmaf with
  `CC=gcc CXX=icpx` and `-Db_lto=false`; the SYCL kernels and the other C++
  code stay with icpx. When the C++ compiler is icpx and the C compiler is
  not, every C++ translation unit now gets icpx's strict floating-point
  options (`-fp-model=precise -ffp-contract=off`), as in an all-icx build, and
  with SYCL enabled the test executables link as C++. CPU scores are those of
  a GCC build, and the single-threaded CPU path is 5-11 % faster than an icx
  build at 1080p (`vmaf_v0.6.1`, `vmaf_float_v0.6.1`, `cambi`)
  ([ADR-1714](docs/adr/1714-hybrid-gcc-cpu-icpx-sycl.md)).
