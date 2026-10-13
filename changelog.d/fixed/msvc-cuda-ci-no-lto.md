- CI: the `Windows MSVC+CUDA` legs of `libvmaf-build-matrix.yml` and `build.yml`
  build with `-Db_lto=false`. With the project default `b_lto=true` every test
  program linked with `/LTCG` against the static libraries, and the link phase
  alone took 32 to 52 minutes of the 60-minute job, which timed out on master.
  The Windows bundles already built without LTO; the project default is unchanged.
