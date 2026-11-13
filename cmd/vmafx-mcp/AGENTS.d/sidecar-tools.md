---
paths:
  - cmd/vmafx-mcp/impl_sidecar*.go
  - cmd/vmafx-mcp/sidecar_parity_test.go
  - pkg/libvmaf/paths.go
  - mcp-server/vmaf-mcp/src/vmaf_mcp/server.py
invariant: Byte-identical sidecar argv + FormatFloat/-1; resolve binaries via libvmaf.FindSidecarBinary.
---
<!-- markdownlint-disable MD013 MD029 -->
# Sidecar tools argv parity and binary resolution (ADR-1184)

## Rebase-sensitive invariants

16. **Sidecar parity required** (ADR-1184, #1240). 15 classic tools and 4 sidecar tools (`vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`) have byte-compatible Python twins. `impl_sidecar.go` `buildPerShotArgv` / `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` MUST produce same argv as `server.py` `_build_per_shot_argv` / `_build_roi_argv` / `_build_bench_argv` / `_build_vpl_argv`; schemas (names, enums, defaults, bounds) must match. `sidecar_parity_test.go::TestSidecarArgvParity` drives both sides, compares. Argv builders split out of handlers so test runs without sidecar binary on disk — do not fold back into handlers. Float arguments formatted with `strconv.FormatFloat(v, 'f', -1, 64)` (Go) and `server.py::_fmt_float` (Python); Python `repr` alone NOT equivalent (keeps trailing `.0` on integral values, switches to exponent notation for small magnitudes; changes argv bytes). New float parameter must go through `_fmt_float`.

18. **Sidecar binary resolution goes through `libvmaf.FindSidecarBinary`** (`pkg/libvmaf/paths.go`). Second candidate — sibling of resolved `vmaf` binary — load-bearing: single `VMAF_BIN` resolves whole family (required by vmaf-dev-mcp container after `make install`). `server.py::_sidecar_binary` mirrors order. Adding sidecar requires adding to `SidecarBinaryEnv` AND `_SIDECAR_BINARY_ENV`.
