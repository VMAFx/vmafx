- **`dev/scripts/smoke-probe-loop.sh` passed vmaf CLI flags that do not
  exist** (`--cuda`, `--sycl`, `--hip`, `--no_prediction_flags`), so every
  GPU/HIP probe iteration of the dev-MCP container's smoke-probe loop
  silently failed. Restored `--backend=cuda` / `--backend=sycl` /
  `--backend=hip` and `--no_prediction` to match `core/tools/cli_parse.cpp`.
  Added `scripts/ci/check-smoke-probe-flags.sh` (wired into
  `.pre-commit-config.yaml`) to catch this class of drift without a GPU.
