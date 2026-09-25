# Research-2118 — BUG-048 script environment drift

## Finding

Two current scripts still encoded environments that no longer exist:

- `testdata/bench_all.sh` fell back to the retired
  `/home/kilian/dev/vmaf` checkout when invoked outside a Git working
  directory. That made the same tracked script machine-specific even though
  its location already identifies the repository root. Its operator-facing
  comments also still claimed that the removed Vulkan backend was part of the
  active benchmark matrix, and the linked `core/AGENTS.md` invocation table
  repeated that stale command. The harness header also named the old
  `1080p_5f` fixture even though the executable path now uses the 1080p
  checkerboard pair, and two comments disagreed about SYCL's current metrics-key
  count (`~12` and `~15` instead of the observed `~34` intermediates).
- `collect_gpu_calibration_data.py` defaults to CUDA and accepts only CUDA or
  SYCL, but its `--arch-id` help still said the default targeted Mesa
  lavapipe. Its manifest regression fixture also used the removed Vulkan
  backend and a deleted `vulkan_device` argument. Its untyped scorer-output
  loader also indexed `json.load()` directly, so malformed output failed later
  with incidental indexing/type errors instead of a bounded schema diagnostic.

## Resolution

The benchmark harness now derives its default root from `BASH_SOURCE[0]`, keeps
`VMAF_ROOT` as the explicit override, and describes only its live CPU/CUDA/SYCL
matrix. The backend-engagement table it cites names the same live set. The
header names the checkerboard fixture the script executes, and both engagement
comments use the current per-backend key-count shape. The calibration help and
fixture now describe CUDA, while retaining SYCL as the other selectable backend.
The frame loader validates the top-level object, the `frames` array, and each
frame object before pairing metrics.
The source-ADR citation registry was regenerated after the four obsolete
ADR-0726 sites left the shell harness.

The regression test rejects the retired absolute checkout and backend name,
requires script-relative root discovery, syntax-checks the shell harness, and
rejects lavapipe in the current CLI help. A hermetic fake `vmaf` invocation
also runs the harness from an unrelated working directory and proves all nine
calls execute from the tracked repository root. It performs no real scoring,
benchmarking, or training.

## Verification

```bash
python3 -m pytest -q ai/tests/test_legacy_extractor_manifests.py
mypy --config-file pyproject.toml \
  ai/scripts/collect_gpu_calibration_data.py \
  ai/tests/test_legacy_extractor_manifests.py
bash -n testdata/bench_all.sh
shellcheck testdata/bench_all.sh
```

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
