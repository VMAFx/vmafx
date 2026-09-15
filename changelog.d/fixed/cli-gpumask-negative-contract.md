- **`test_vmaf_cuda_gpumask` failed on every host that actually has an
  NVIDIA GPU.** The script — inherited verbatim from upstream — documented
  `--gpumask -1` as "use cpu" and invoked it twice under `set -e`, but the
  fork's CLI rejects negative values for `--gpumask`. It looked green in CI
  only because the script skips (exit 77) when `nvidia-smi -L` finds no
  device. Upstream's `-1` works by accident: POSIX `strtoul` silently turns
  `"-1"` into `ULONG_MAX`, and this fork deliberately refuses a leading `-`
  rather than wrap a value the caller did not mean. Fixed by using
  `--gpumask 1`, the documented "any non-zero value disables the GPU feature
  extractors" spelling — measured byte-identical to `--no_cuda --no_sycl`.
  The `--gpumask` entry in `docs/usage/cli.md` is corrected at the same time:
  it described a per-op bitmask, which the option has never been.
