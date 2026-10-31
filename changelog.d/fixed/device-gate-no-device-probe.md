- **The no-fallback device gate skips a leg with no device before it reads any
  fixture.** The HDR rows made `test_gpu_v1_models_no_fallback.py` fail with
  "fixture files not found" on legs that have neither a device nor the
  fixtures, because the check that the CLI refuses the backend only ran inside
  a row. A probe on a blank picture now settles that first; with a device
  present, an absent fixture still fails the gate. The macOS Metal CI leg now
  fetches the gate's YUV pairs before running the meson tests so device-resident
  HDR runs find their fixtures.
