- **The upstream FFmpeg compat check runs SVT Encore's `libvmaf` graph (#2364).**
  `scripts/ci/upstream-ffmpeg-compat.sh --against-cli` builds the filter graph
  of SVT Encore (`CommandBuilder.vmafParams()`, commit `8dd7c596c51a`) for each
  profile option set (models joined by `|`, `feature`, `n_threads`,
  `n_subsample`, reference filters) against stock FFmpeg and compares every
  result with the `vmaf` CLI; an unknown filter option must fail by name. See
  [upstream consumers](docs/development/upstream-consumers.md).
