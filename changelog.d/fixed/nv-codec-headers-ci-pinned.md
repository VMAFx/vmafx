- CI: the hosted CUDA legs of `build.yml` and `libvmaf-build-matrix.yml` install
  nv-codec-headers at the commit `build-config.env` pins, through
  `scripts/ci/fetch-nv-codec-headers.sh`, instead of cloning the default branch
  from one host. The script retries, falls back to GitHub's mirror, and fails
  when the fetched commit is not the pin or lacks the loader header.
