# Research digest: MCP sidecar-binary and gRPC control-plane bridge (2026-09-05)

## Scope

The remaining items of epic #1240: bridging the four sidecar CLI binaries into
the MCP tool surface, and the Priority-3 gRPC control-plane bridge. This digest
records what was read to fix each tool's parameter surface, and the three
findings that changed the design.

## Source of truth for every bound

Nothing here was taken from the sidecars' `--help` text alone; every bound comes
from the C parser, because several help strings understate them.

| Tool | Parser read | Bounds it enforces |
| --- | --- | --- |
| `vmaf_per_shot` | `core/tools/vmaf_per_shot.c::per_shot_apply_opt`, `per_shot_validate`, `per_shot_settings_defaults` | width/height `16..65535`; bitdepth ∈ {8,10,12,16}; `--target-vmaf` `0..100`; `--crf-min`/`--crf-max` `0..63` with `crf_min <= crf_max`; `--diff-threshold` `0..255` (default 12.0); `--format` csv\|json (C default **csv**) |
| `vmaf_roi` | `core/tools/vmaf_roi.c::parse_args_num_opt`, `parse_args_str_opt`, `opts_set_defaults`, and the `VMAF_ROI_*` defines | width/height `1..16384`; `--frame` `0..1000000`; `--ctu-size` `8..128`; `--strength` `0..64` (a `double`); bitdepth restricted to {8,10,12,16} by an explicit second check after the `8..16` range check |
| `vmaf_bench` | `core/tools/vmaf_bench.c::parse_bench_opt`, `parse_resolution_arg`, `print_bench_help`, `main` | `--resolution` is one of exactly five strings; `--bpc` ∈ {8,10,12,16}; `--frames` is **clamped, not rejected** (`<2 → 2`, `>48 → 48`) |
| `vmaf_vpl` | `core/tools/vmaf_vpl.c::print_usage`, the argv loop, and the score/summary `printf`s | `--frames` `0 = all`; `--device` index; `--render-node` is a device path handed straight to `open(2)` |

## Finding 1 — three sidecar outputs are not JSON, and one is not text

`vmaf_per_shot --format json` writes a clean JSON document to stdout when
`--output -`, so it parses. The other three do not:

- `vmaf_roi` with `--encoder x265` writes a **qpfile**: two `#` comment lines
  then one space-separated row of signed CTU offsets per grid row.
- `vmaf_roi` with `--encoder svt-av1` writes a **raw row-major `int8` grid with
  no header**, and `emit_sidecar` opens the file `"wb"` for that case. Routing it
  through stdout would corrupt it, so both servers always write to a temp file
  and choose the response encoding from the emitter: `qpfile` (text) or
  `roi_map_base64`.
- `vmaf_bench` writes a fixed-width text table; `vmaf_vpl` writes
  `Frames: N` / `Time: … s (… FPS)` / `VMAF:   <score> (mean)` lines. The VPL
  summary is regex-parsed into `vmaf_score` / `frames_processed` and the raw
  stdout is returned alongside.

## Finding 2 — `vmaf_bench --validate` exits 1 as a *result*

`run_bench_validation_mode` returns `total_fail > 0 ? 1 : 0` and prints
`ALL PASSED` / `FAILURES`. Treating that exit code as a tool error (which is what
`run_benchmark` does for `bench_all.sh`, ADR-0608 E-1) would report a successful
GPU-vs-CPU comparison that found deltas as a broken tool. So `vmaf_bench` splits
the two: in `validate` mode a non-zero exit becomes
`"validation_failed": true` on a successful call; in benchmark mode it is an
error, matching every other sidecar.

## Finding 3 — Go and Python disagree on how to print a float

The parity contract is byte-identical argv. Go writes floats as
`strconv.FormatFloat(v, 'f', -1, 64)`: the shortest decimal that round-trips,
never exponent notation. Python's `repr` is also shortest-round-trip but differs
in two places that matter here:

| Value | Go `'f', -1` | Python `repr` |
| --- | --- | --- |
| `90.0` | `90` | `90.0` |
| `1e-05` | `0.00001` | `1e-05` |

Both differences reach the argv (`--target-vmaf 90` vs `--target-vmaf 90.0`).
`server.py::_fmt_float` therefore strips a trailing `.0` and re-renders an
exponent form through `decimal.Decimal(...)`, `format(..., "f")`.
`sidecar_parity_test.go` carries a dedicated `per_shot_integral_float_formatting`
case for it.

## gRPC bridge: what the existing building blocks already provide

- `cmd/vmafx-controller/proto/controller.proto` has `SubmitJob`, `GetJob`,
  `CancelJob` and `StreamJobs` on the Client API; `list_jobs` maps onto
  `StreamJobs`, which per [ADR-0962](../adr/0962-controller-streamjobs-and-reaper-stop.md)
  streams the current SQLite snapshot and **closes** — draining it to `io.EOF`
  terminates, so no subscription bookkeeping is needed.
- `backend: "auto"` has no wire representation: `ScoringParams.backend`'s
  documented "empty string means the scheduler may assign any available node", so
  the bridge normalises `auto → ""`.
- `pkg/score.Client.Score` already wraps the unary `VmafxScoring.Score` RPC,
  including the OTel client stats handler, so `vmaf_score_remote` is a thin call
  over it rather than a second dial path.
- The controller's HTTP surface (`cmd/vmafx-controller/http_server.go`) exposes
  only `/healthz`, `/readyz`, `/metrics` and `/v1/score` — **no job endpoints**.
  That rules out "let the Python server use HTTP instead of gRPC" without
  designing a second control-plane API, and is one of the inputs to
  [ADR-1184](../adr/1184-mcp-grpc-bridge-go-only.md).

## Verification

Both servers were driven end-to-end against the host CPU build
(`core/build/tools/`) and produced identical results for the same inputs — e.g.
`vmaf_roi` on `testdata/ref_576x324_48f.yuv` frame 2: a 9×6 grid, 216 qpfile
bytes / 54 `int8` bytes, same offsets. The five gRPC tools were driven against a
locally started `vmafx-controller` (`VMAFX_AUTH_DISABLED=true`,
`VMAFX_GRPC_LISTEN=127.0.0.1:59090`): submit → get → list → cancel → get showed
the job transitioning `PENDING → CANCELLED`, an unknown ID surfaced the
controller's `NotFound`, and `vmaf_score_remote` reached the server's `Score` RPC
and returned its server-side error.
