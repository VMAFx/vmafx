# BUG-048 script environment drift

## Finding

Two current scripts still encoded environments that no longer exist:

- `testdata/bench_all.sh` fell back to the retired
  `/home/kilian/dev/vmaf` checkout when invoked outside a Git working
  directory. That made the same tracked script machine-specific even though
  its location already identifies the repository root.
- `collect_gpu_calibration_data.py` defaults to CUDA and accepts only CUDA or
  SYCL, but its `--arch-id` help still said the default targeted Mesa
  lavapipe. Its manifest regression fixture also used the removed Vulkan
  backend and a deleted `vulkan_device` argument.

## Resolution

The benchmark harness now derives its default root from
`BASH_SOURCE[0]` and keeps `VMAF_ROOT` as the explicit override. The
calibration help and fixture now describe CUDA, while retaining SYCL as the
other selectable backend.

The regression test rejects the retired absolute checkout, requires
script-relative root discovery, syntax-checks the shell harness, and rejects
lavapipe in the current CLI help. It does not execute a benchmark or train a
model.

## Alternatives considered

| Alternative | Benefit | Defect | Decision |
| --- | --- | --- | --- |
| Keep an absolute fallback | Works on one historical workstation | Fails in worktrees, containers, source archives, and other clones | Rejected |
| Fall back to the caller's current directory | Short shell expression | An invocation from another directory selects unrelated files | Rejected |
| Resolve from the script path | Stable for every checkout and worktree | Assumes the tracked `testdata/` layout | Chosen |

No ADR is needed: this restores the existing portable-root and live-backend
contracts without choosing a new architecture or policy. No public C API,
Netflix golden assertion, FFmpeg patch, benchmark result, model, or training
output changes.
