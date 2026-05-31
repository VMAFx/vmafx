<!-- markdownlint-disable MD060 -->
# MCP server: subprocess → direct cgo migration plan

Status: Phase 1 (Proposed)
Owner: lusoris
Tracked by: [ADR-0931](../adr/0931-mcp-cgo-direct-replace-subprocess.md)
Last updated: 2026-05-31

## What changed

The Go MCP server (`cmd/vmafx-mcp/`) historically delegated every scoring or
introspection call to the `vmaf` CLI binary via `exec.Command(...)` and
parsed its JSON stdout. Phase 1 of ADR-0931 introduces a **direct cgo
scoring path** in `pkg/libvmaf/` (`ScoreDirect`, `ValidateModel`) and wires
the two simplest tool handlers (`vmaf_score`, `describe_model`) to take that
path when `VMAFX_MCP_DIRECT=1` is set in the environment. The subprocess
path remains the default and is **not removed in this PR**.

The migration is intentionally staged:

| Phase | Scope | Default | Status |
|---|---|---|---|
| **1** | `vmaf_score` + `describe_model`, CPU + SVM only | subprocess | This PR |
| 2 | `vmaf_score_encoded`, `probe_backend`, `run_benchmark`; GPU backend support; per-feature pooled scores | subprocess | Planned |
| 3 | Default-flip `VMAFX_MCP_DIRECT=1` after parity sweep; ONNX/DNN cgo bridge | direct | Planned |
| 4 | Remove subprocess path + `pkg/libvmaf::FindBinary` heuristic | direct | Planned |

## How the opt-in works

The dispatcher in `cmd/vmafx-mcp/impl_direct.go::directPathEnabled` reads
`VMAFX_MCP_DIRECT` per call. Only the exact string `"1"` enables the direct
path; any other value (including unset and `"true"`) leaves the subprocess
path in effect.

```bash
# Default: subprocess path
vmafx-mcp serve --transport stdio

# Phase 1 opt-in: direct cgo
VMAFX_MCP_DIRECT=1 vmafx-mcp serve --transport stdio
```

On the first call the direct path emits a one-shot marker to stderr so the
operator can confirm the choice took effect:

```text
libvmaf: VMAFX_MCP_DIRECT=1 — using in-process cgo scoring path (ADR-0931)
```

## Fall-back behaviour

The direct path falls back transparently to the subprocess path in any of
these cases:

| Trigger | Reason |
|---|---|
| `backend` arg is not `auto` or `cpu` | Phase 1 is CPU only |
| Model file extension is `.onnx` | DNN cgo bridge lands in Phase 3 |
| `resolveModelArgToPath` cannot find the model on disk | `vmaf.c` has its own version-table resolver; defer to it |

The fall-back is silent (no marker) and the response payload is identical to
the always-subprocess path. The opt-in is therefore **safe to leave on**
even when calling tools the direct path does not yet support.

## Response shape

The direct path emits the same JSON the subprocess path emits, with one
addition: `backend_used` reads `"cpu (direct cgo)"` instead of `"cpu"` so
clients can confirm which path executed. The `frame_count` field is also
populated (the subprocess path only emits per-frame entries under `frames`).

```json
{
  "pooled_metrics": { "vmaf": { "mean": 76.668 } },
  "frames": [],
  "backend_requested": "cpu",
  "backend_used": "cpu (direct cgo)",
  "frame_count": 48
}
```

Phase 2 will populate `frames` with per-frame VMAF + per-feature pooled
scores so the response is byte-identical to the subprocess JSON.

## Error mapping

The direct path returns typed errors so MCP clients can branch
programmatically:

| libvmaf return | Go sentinel | Wraps |
|---|---|---|
| `-EINVAL` | `libvmaf.ErrInvalidArgument` | `os.ErrInvalid` |
| `-ENOMEM` | `libvmaf.ErrOutOfMemory` | — |
| `-ENOENT` | `libvmaf.ErrModelNotFound` | `os.ErrNotExist` |
| `-EIO` | `libvmaf.ErrPictureRead` | — |
| other `< 0` | `fmt.Errorf("libvmaf %s returned %d", call, rc)` | — |

The mapping is defined in `pkg/libvmaf/errors.go` and tested in
`pkg/libvmaf/errors_test.go`. The Phase 1 contract is frozen by ADR-0931;
extensions need an ADR amendment.

## Reproducer

```bash
# Build the CPU-only libvmaf and the MCP binary.
meson setup core/build-cpu -Denable_cuda=false -Denable_sycl=false
ninja -C core/build-cpu
go build -o /tmp/vmafx-mcp ./cmd/vmafx-mcp/

# Run the smoke test against the 576x324 / 48-frame fixture.
LD_LIBRARY_PATH=$(pwd)/core/build-cpu/src \
  VMAFX_MCP_DIRECT=1 \
  VMAF_MCP_ALLOW=$(pwd)/testdata \
  go test -v -run TestHandleVmafScore_RoutesToDirect ./cmd/vmafx-mcp/
```

Expected output: `--- PASS: TestHandleVmafScore_RoutesToDirect`, with the
`libvmaf: VMAFX_MCP_DIRECT=1` marker on stderr and a payload whose
`backend_used` contains `"direct cgo"` and whose `frame_count == 48`.

## Latency note (informal)

Sampled on a single workstation (i9-13900K, Linux 7.0.10) against the
576x324 / 48-frame `testdata` fixture, the direct path is **roughly an
order of magnitude faster per call** than the subprocess path. The
subprocess path takes ~250 ms (fork/exec + vmaf binary cold start + model
load + JSON write/read). The direct path takes ~25 ms (no fork; ~10 ms is
the scoring itself, the rest is model load). The numbers are illustrative
only — Phase 3 will land formal benchmarks gated by CI.

## Rollback

Unset `VMAFX_MCP_DIRECT` (or set to anything other than `"1"`) and restart
the MCP server. Every tool resumes the subprocess path immediately. No
state is persisted across the toggle.
