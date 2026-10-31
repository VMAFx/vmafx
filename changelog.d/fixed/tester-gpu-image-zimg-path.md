- **The GPU tester images find libzimg again.** Since zimg became a default dependency, the GPU
  build stages of `docker/Dockerfile.tester` stage `libzimg.so.2` in `/opt/vmafx/lib`, but the
  NVIDIA runtime stage did not copy that directory and none of the three GPU runtime stages had it
  on `LD_LIBRARY_PATH`, so all three GPU image builds stopped at their `ldd` checks. The runtime stages now
  copy and search it; `tools/rc1-tester/tests/test_dockerfile_runtime_libs.py` checks the contract
  on every pull request.
