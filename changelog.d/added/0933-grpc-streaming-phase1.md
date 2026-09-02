## Added

- **`ScoreStream` bidirectional gRPC RPC** (`proto/vmafx.proto`,
  `cmd/vmafx-server/grpc_server.go`, `pkg/score/grpc_client.go`,
  `gen/go/`): Phase 1 of multi-frame streaming scoring per ADR-0933.
  Adds `rpc ScoreStream(stream ScoreStreamRequest) returns (stream ScoreStreamResponse)`
  alongside the existing unary `Score` RPC (v1 API preserved unchanged).
  The new RPC carries a leading `StreamConfig` plus N `FramePair` messages
  with raw planar Y/U/V bytes, and emits per-frame `FrameScore` messages
  plus a terminal `AggregateScore` on a `oneof`-typed response. Phase 1
  ships the proto schema, regenerated Go bindings, a server handler stub
  that validates framing and returns `codes.Unimplemented`, a
  `pkg/score` client wrapper that hides the `oneof` framing behind a
  `PushFrame` / `Recv` API, smoke tests, and the architecture doc at
  `docs/architecture/grpc-streaming.md`. Phase 2 will wire the handler
  to libvmaf via an in-memory picture-import path.
