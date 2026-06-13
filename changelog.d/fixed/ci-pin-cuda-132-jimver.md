- **CI**: pin Jimver/cuda-toolkit action CUDA version back to 13.2.0 in `build.yml` and
  `libvmaf-build-matrix.yml`. The 13.3.0 bump in PR #664 is valid for apt-based
  installs (dev/Containerfile, Dockerfile stay at 13.3) but the Jimver action v0.2.35
  does not carry 13.3.0 in its installer index, causing "Version not available: 13.3.0"
  on every GitHub-hosted Linux and Windows CUDA runner.
