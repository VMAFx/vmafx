- `ffmpeg-integration.yml` is shellcheck-clean. The `SC2317` "command appears
  unreachable" notes on its SYCL build step were a false positive that fired
  only on machines with oneAPI installed: actionlint passes
  `--external-sources`, so shellcheck inlined `/opt/intel/oneapi/setvars.sh`,
  whose every top-level branch ends in `return`, and modelled that as
  terminating the calling script rather than returning to it. The step now
  reaches setvars.sh the way the `clang-tidy-sycl` job already does, and four
  unquoted command substitutions in the same file (`SC2046` / `SC2086`) are
  quoted.
