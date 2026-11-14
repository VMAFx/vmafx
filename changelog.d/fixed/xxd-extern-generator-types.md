- Built-in JSON and BRISQUE models and HIP kernel blobs now declare the exact
  `unsigned char[]` and `unsigned int` types emitted by `xxd -i`. Read-only
  access remains const-qualified at use sites. The CPU build compiles model
  declarations and generated definitions together in `test_xxd_extern_types`
  to reject future type mismatches; run it with
  `python3 scripts/ci/run_meson_test.py -- -C build test_xxd_extern_types`.
  HIP builds with generated kernels also compile every HSACO declaration and
  definition together in `test_xxd_hsaco_extern_types`.
