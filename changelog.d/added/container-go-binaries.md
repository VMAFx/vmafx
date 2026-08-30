- **The dev container now ships the six Go binaries.** `dev/Containerfile` had no
  Go toolchain at all, so `vmafx-server`, `vmafx-mcp`, `vmafx-tune`,
  `vmafx-controller`, `vmafx-node` and `vmafx-operator` existed only on a
  developer's host — which made CLAUDE.md §15 ("default to the container for
  vmaf / vmaf-tune / ai / MCP-probing work") impossible to satisfy for any Go
  surface, and blocked the Python sunset in ADR-0703 §Decision / ADR-0704
  §Consequences, since the Python implementations cannot be removed from the
  image before the Go replacements are in it. A `go-build` stage now compiles
  all six into `/usr/local/bin`, beside the C `vmaf` CLI.
- The stage derives from `libvmaf-build` and copies the Go toolchain in, rather
  than deriving from a bare `golang` image: four of the six binaries link
  libvmaf through cgo (`pkg/libvmaf`) and fail under `CGO_ENABLED=0` with
  `undefined: libvmaf.Scorer`. Only `vmafx-tune` and `vmafx-operator` are pure
  Go. The build asserts all six binaries are produced.
