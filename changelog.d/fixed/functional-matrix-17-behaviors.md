# fix: functional-matrix failures — 17 tool/backend behaviors

Fixed a set of real behavioral regressions identified by the full-matrix
validation run. Items 1, 2, 16, 17 (HIP VIF wavefront carry-drop) were
already resolved in `vif_statistics.hip` (ADR-0563, per-thread atomicAdd);
the remaining items are fixed here.

## Changes

- **bench_all.sh** (`testdata/`): removed the `run "${tag}_vulkan"` call and
  the Vulkan entry in the comparison table that referenced the undefined
  `$FLAGS_VULKAN` variable, causing a fatal `unbound variable` error under
  `set -u` (ADR-0726 follow-up).

- **bisect.py** (`tools/vmaf-tune`): `_workdir_parent()` now checks
  writability of the `VMAFTUNE_WORKDIR` path via `os.access(path, os.W_OK)`
  and falls back to `None` (OS `/tmp`) when the path is not writable,
  preventing `PermissionError` crashes in tune-per-shot and compare when
  `/probes/vmaftune-work` is bind-mounted read-only.

- **server.py** (`mcp-server/vmaf-mcp`): `_run_tune_per_shot` no longer
  passes `--format <format>` to `vmaf-tune tune-per-shot`; the subcommand
  does not accept that flag and was exiting with argparse code 2. Stdout is
  always parsed as JSON.

- **cli.py** (`tools/vmaf-tune`): `_run_recommend_saliency` now detects
  when `--output` ends in `.json` and redirects the encode to a sibling
  `<stem>_encoded.mp4`, writing the JSON result to the requested path.
  Eliminates exit-status 234 / EINVAL from ffmpeg receiving a `.json` path
  as its output muxer target.

- **op_allowlist.c** (`core/src/dnn`): added `DynamicQuantizeLinear`,
  `MatMulInteger`, and `ConvInteger` to the allowlist, unblocking dynamic-PTQ
  int8 models (`vmaf_tiny_v3.int8`, `vmaf_tiny_v4.int8`, `nr_metric_v1.int8`)
  from loading via the C op-scanner.

- **float_adm_cuda.c** (`core/src/feature/cuda`): added an explicit
  `cuStreamSynchronize(s->lc.str)` in `collect_fex_cuda` after
  `vmaf_cuda_kernel_collect_wait` to eliminate a race between frame N's
  D2H copy (on `lc.str`) and frame N+1's `cuMemsetD8Async` (on `pic_stream`).
  Fixes catastrophic ADM score corruption (~31% of frames) in the
  `hwupload_cuda` pipeline.

- **dnn_api.c** (`core/src/dnn`): `vmaf_dnn_session_open` now treats
  symbolic/dynamic batch dimensions (ORT returns 0 or negative for these)
  as single-frame (N=1), allowing models with `['batch', 1, H, W]` input
  shapes to use the luma fast-path. `vmaf_dnn_session_run_luma8` now
  reallocates scratch buffers on frame-size mismatch instead of returning
  `-ERANGE`, allowing fixed-shape models to handle differently-sized inputs.

- **Containerfile** (`dev/`): installs `vmaf-tune[fast]` (includes `optuna`)
  so the `fast` subcommand works inside the canonical dev container.
