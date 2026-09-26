# Research-2118 — BUG-048 script environment drift

## Finding

Two current scripts and both MCP wrappers still encoded environments that no
longer exist:

- `testdata/bench_all.sh` fell back to the retired
  `/home/kilian/dev/vmaf` checkout when invoked outside a Git working
  directory. That made the same tracked script machine-specific even though
  its location already identifies the repository root. Its operator-facing
  comments also still claimed that the removed Vulkan backend was part of the
  active benchmark matrix, and the linked `core/AGENTS.md` invocation table
  repeated that stale command. The harness header also named the old
  `1080p_5f` fixture even though the executable path now uses the 1080p
  checkerboard pair. Two comments also froze incompatible metrics-key counts;
  later measurements changed again, proving that backend counts are evidence
  attached to one run rather than stable backend constants.
- `collect_gpu_calibration_data.py` defaults to CUDA and accepts only CUDA or
  SYCL, but its `--arch-id` help still said the default targeted Mesa
  lavapipe. Its manifest regression fixture also used the removed Vulkan
  backend and a deleted `vulkan_device` argument. Its untyped scorer-output
  loader also indexed `json.load()` directly, so malformed output failed later
  with incidental indexing/type errors instead of a bounded schema diagnostic.
- The Python and Go MCP wrappers repeated the stale-count assumption in their
  backend-inference helpers: they called 12-or-fewer metric keys `gpu` and
  larger payloads `cpu`. Current CUDA output can contain 14 keys, so a
  successful CUDA auto-dispatch could be relabelled as CPU even though the
  fork's CLI already writes an authoritative top-level `backend_used` receipt.
  Non-object JSON also reached incidental failures; Go's `null` case could
  panic while annotating a nil map.

## Resolution

The benchmark harness now derives its default root from `BASH_SOURCE[0]`, keeps
`VMAF_ROOT` as the explicit override, and describes only its live CPU/CUDA/SYCL
matrix. The backend-engagement table it cites names the same live set. The
header names the checkerboard fixture the script executes. The harness records
the emitted count for each row and marks a GPU count equal to CPU as
`FALLBACK-SUSPECT`, while documentation treats counts as dated observations
rather than permanent expectations. The calibration help and fixture now
describe CUDA, while retaining SYCL as the other selectable backend.
The frame loader validates the top-level object, the `frames` array, and each
frame object before pairing metrics.
For MCP auto scoring, both wrappers now accept the concrete `backend_used`
receipt emitted by the CLI and return `unknown` when an older or external
binary omits it or supplies an invalid value. Neither classifies a backend by
metric count. MCP score JSON must be a top-level object before either wrapper
adds response metadata. Go's direct-cgo path remains separate and keeps its
explicit `cpu (direct cgo)` receipt.
The source-ADR citation registry was regenerated after obsolete ADR-0726 sites
left the shell harness and both count-heuristic implementations.

The regression test rejects the retired absolute checkout, backend name, and
fixed key-count contracts; requires script-relative root discovery; and
rejects lavapipe in the current CLI help. A hermetic fake `vmaf` invocation
runs the harness from an unrelated working directory, proves all nine calls
execute from the tracked repository root, and exercises the equal-count
fallback warning. It performs no real scoring, benchmarking, or training.
Python and Go MCP red caps pair the dated CPU 15 / CUDA 14 / SYCL 24
observations with explicit CLI receipts, proving the receipt wins without
freezing the counts; missing and invalid receipts resolve to `unknown`, and
top-level non-object score JSON is rejected. Go also proves an explicit backend
request still wins and the direct-cgo receipt remains intact.

## Verification

```bash
python3 -m pytest -q ai/tests/test_legacy_extractor_manifests.py
mypy --config-file pyproject.toml \
  ai/scripts/collect_gpu_calibration_data.py \
  ai/tests/test_legacy_extractor_manifests.py
bash -n testdata/bench_all.sh
shellcheck testdata/bench_all.sh
nox -s mcp -- -k 'bug1_auto or infer_backend_from_payload or score_payload_rejects'
CGO_LDFLAGS=-L"$PWD/core/build-cpu/src" \
LD_LIBRARY_PATH="$PWD/core/build-cpu/src" \
  go test ./cmd/vmafx-mcp -run \
  '^(TestInferBackendFromPayload|TestDecodeVmafOutput.*|TestHandleVmafScore_RoutesToDirect)$'
```

## Alternatives considered

| Alternative | Benefit | Defect | Decision |
| --- | --- | --- | --- |
| Keep an absolute fallback | Works on one historical workstation | Fails in worktrees, containers, source archives, and other clones | Rejected |
| Fall back to the caller's current directory | Short shell expression | An invocation from another directory selects unrelated files | Rejected |
| Resolve from the script path | Stable for every checkout and worktree | Assumes the tracked `testdata/` layout | Chosen |
| Infer MCP backend identity from metric count | Works with old binaries lacking a receipt | Counts move with extractor registration and can mislabel successful GPU runs | Rejected |
| Trust a validated CLI receipt, otherwise report `unknown` | Uses direct runtime evidence and fails closed when absent | Older binaries lose a guessed label | Chosen |

No ADR is needed: this restores the existing portable-root and live-backend
contracts without choosing a new architecture or policy. The existing MCP
response field now consumes the CLI receipt that was already designed for this
purpose in both server implementations; its schema is unchanged. No public C
API, Netflix golden assertion, FFmpeg patch, benchmark result, model, or
training output changes.
