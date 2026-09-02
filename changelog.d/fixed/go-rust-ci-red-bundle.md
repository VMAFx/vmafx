- **Go CI** (`go-ci.yml`): `go test ./...` failed on every package that links
  `libvmaf.so.3` via cgo (`vmafx-controller`, `vmafx-mcp`, `vmafx-node`,
  `vmafx-server`, `pkg/libvmaf`) with "cannot open shared object file" because
  `core/build-cpu/src/` was not on `LD_LIBRARY_PATH` at test runtime. Fixed by
  adding `LD_LIBRARY_PATH: ${{ github.workspace }}/core/build-cpu/src` to the
  `go test` step env.
- **Go CI** (`vmafxnode_controller_test.go`): `VmafxNode controller — sets Healthy =
  false when LastHeartbeat is stale` failed with timestamp precision mismatch.
  `metav1.NewTime(time.Now())` captures nanoseconds; the Kubernetes API server
  stores `metav1.Time` as RFC3339 (second granularity) and truncates sub-second
  precision on read-back. Fixed by truncating `staleTime` to second precision with
  `.Truncate(time.Second)` before the `Expect(Equal(...))` assertion.
- **Go CI** (`vmafx-mcp/impl_direct.go`): `TestResolveModelArgToPath_AllowlistEnforced`
  failed because `resolveModelArgToPath` accepted absolute paths outside the
  allowlisted roots without routing them through `libvmaf.ValidatePath`. PR #791
  (context propagation fix) inadvertently removed the `ValidatePath` calls added
  by PR #813, reopening the security regression. Restored `libvmaf.ValidatePath`
  for all resolved path candidates (absolute, relative, and bare-stem lookups).
- **Rust CI** (`vmafx-sys/Cargo.toml`): `cargo test -p vmafx-sys --all-features`
  ran doc-tests against bindgen-generated `bindings.rs` which contains C header
  comments verbatim ("On x86 / x86_64:" text, backtick-quoted function names).
  Rust doc-tests failed to compile the extracted code snippets. Fixed by adding
  `[lib] doctest = false` to `vmafx-sys/Cargo.toml` — the standard convention
  for `*-sys` crates where the lib surface is machine-generated FFI.
