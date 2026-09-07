- **Nine new MCP tools** on both `vmafx-mcp` (Go) and `vmaf-mcp` (Python), closing
  the remaining items of epic #1240.
  - **Sidecar-binary bridge (both servers)**: `vmaf_per_shot`, `vmaf_roi`,
    `vmaf_bench` and `vmaf_vpl` expose the four CLI binaries meson builds next to
    `vmaf` in `core/tools/`. Every numeric and enum bound is taken from the
    corresponding C parser, so an out-of-range value returns a readable MCP error
    instead of a `usage()` dump; structured output is parsed (the per-shot JSON
    plan, the VPL score summary) and the ROI sidecar comes back as qpfile text for
    x265 or base64 for the SVT-AV1 `int8` map. Binaries resolve from a per-tool
    env override (`VMAF_PER_SHOT_BIN`, `VMAF_ROI_BIN`, `VMAF_BENCH_BIN`,
    `VMAF_VPL_BIN`), then a sibling of the resolved `vmaf` binary, then
    `/usr/local/bin`, then the in-tree build dirs.
  - **Phase-4b gRPC control-plane bridge (Go only, ADR-1184)**: `submit_job`,
    `get_job`, `cancel_job`, `list_jobs` and `vmaf_score_remote` drive the
    `vmafx-controller` job API and the `vmafx-server` scoring API. Connection
    targets and credentials are environment-only — `VMAFX_CONTROLLER_ADDR`,
    `VMAFX_SERVER_ADDR`, `VMAFX_CONTROLLER_TOKEN`, `VMAFX_GRPC_TIMEOUT` — so no
    tool argument can name a host.
  - Documented per tool and per parameter in `docs/mcp/tools.md`.
- **`pkg/libvmaf.FindSidecarBinary` and `pkg/libvmaf.ValidateDir`**: sidecar
  binary resolution, and the directory-shaped counterpart of `ValidatePath` that
  `vmaf_bench --data-dir` needs (`ValidatePath` requires a regular file).
