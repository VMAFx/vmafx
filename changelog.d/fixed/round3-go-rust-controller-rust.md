### Fixed

Round-3 bug-hunt — Go controller + Rust bindings (all off the metric scoring path; golden-safe).

- **Controller never propagated the score (R3-1, high)**
  (`cmd/vmafx-controller/grpc_server.go`): `queueJobToProto` omitted
  `FinalScore`, so every *successful* job returned `FinalScore=0` over gRPC and
  the operator wrote `VmafxJob.Status.Score = 0`. The applystatus unit test
  hand-built the proto, hiding the broken producer. Fix: `FinalScore: j.Score`;
  strengthened the integration test to exercise the real
  ReportResult→queue→GetJob→queueJobToProto path.
- **Rust `Context::read_pictures` double-unref → UAF/double-free (R3-2, high)**
  (`bindings/rust/vmafx/src/context.rs`): the wrapper blanket-unref'd both
  pictures on any error, but libvmaf takes ownership on every error path that
  reaches its `cleanup:` label — benign on a CPU build (freed structs are
  memset to NULL) but a use-after-free/double-free against a CUDA-enabled
  libvmaf (the container default), whose `translate_picture` shallow-copies.
  Fix: drop the manual unref (mirror the C ownership contract); add a
  success/error-path liveness regression test.
- **JWKS cache positional truncation (R3-13, low)**
  (`cmd/vmafx-controller/auth/middleware.go`): the cache kept only the first 16
  keys by document order, so a token whose `kid` sat past index 16 was 401'd
  for the 30s refresh cooldown. Fix: build the kid→key map from all parsed keys;
  apply the size bound as eviction of other kids, never the requested one.
- **`examples/score.rs` leaks both pictures on mid-loop read error (R3-14, low)**
  (`bindings/rust/vmafx-sys`): replaced the bare `?` reads with a match that
  unrefs both allocations before propagating, mirroring the clean-EOF path.
