- **`Containerfile.vmafx`: a SYCL + FFmpeg test image with a build-time
  golden gate.** It builds libvmaf (SYCL), FFmpeg at the tag
  `ffmpeg-patches/series.txt` targets with every patch (including the
  `libvmaf_sycl` filter), and runs the Netflix CPU golden tests while it
  builds; a failing golden test fails the build. The Intel GPU stack comes
  from `build-config.env`. `scripts/test/run-all-tests.sh` runs the GPU suites
  against the built image and `scripts/test/reference_report.py` prints CPU
  and SYCL scores next to the Netflix reference values
  ([ADR-1715](docs/adr/1715-vmafx-sycl-ffmpeg-container.md)).
