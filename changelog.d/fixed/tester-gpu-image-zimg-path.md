- **The GPU tester images find libzimg again.** Since zimg became a default dependency, the GPU
  build stages of `docker/Dockerfile.tester` stage `libzimg.so.2` in `/opt/vmafx/lib`, but the
  NVIDIA runtime stage did not copy that directory and none of the three GPU runtime stages had it
  on `LD_LIBRARY_PATH`, so all three GPU image builds stopped at their `ldd` checks. The runtime stages now
  copy and search it; `tools/rc1-tester/tests/test_dockerfile_runtime_libs.py` checks the contract
  on every pull request.
- **The tester images credit the libzimg they ship.** The CPU and GPU tester images copy
  `libzimg.so.2` out of Debian's `libzimg2` package (3.0.5+ds1-1 on the pinned trixie bases), but
  their licence manifest named upstream zimg 3.0.6. Each image now carries the package's Debian
  copyright file and `SOURCE` record under `/usr/share/doc/libzimg2`, and the `zimg` entry of
  `tools/rc1-tester/image/licensing.json` reads them, as the `libgomp1` entry already does.
