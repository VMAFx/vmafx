# AGENTS.md — cmd/vmafx-server

Go gRPC + HTTP scoring service. See [ADR-0703](../../docs/adr/0703-vmafx-server-go-grpc.md).

## Rebase-sensitive invariants

1. **Proto-package stability** (`proto/vmafx.proto`): the proto stays in
   package `vmafx.v1`. The unary `Score` and `Health` RPCs are frozen for
   compatibility; never rename or remove their request / response fields.
   Additive surfaces (new RPCs, new messages, new enum variants) only.
   When breaking changes become necessary, bump the proto package to
   `vmafx.v2` and ship both side-by-side until the v1 deprecation
   window closes — never edit `vmafx.v1` in-place.

2. **`ScoreStream` opening message** (`proto/vmafx.proto` +
   `grpc_server.go`): the first `ScoreStreamRequest` on the bidirectional
   stream MUST set the `config` oneof (`StreamConfig`); every subsequent
   request MUST set `frame_pair`. The server validates this in
   `grpcServer.ScoreStream` and rejects malformed sequences with
   `codes.InvalidArgument` before reading any frame bytes. Per ADR-0933,
   `frame_index` is strictly monotonic from 0 — gaps are an error. The
   Phase 2 implementation MUST preserve this contract so existing
   clients keep working.

3. **`ScoreStream` Phase 1 stub** (`grpc_server.go`): the handler returns
   `codes.Unimplemented` *after* validating the opening `StreamConfig`,
   not before. This is deliberate — it lets clients learn early whether
   they framed the stream correctly without first uploading gigabytes of
   frame data. When Phase 2 wires the real scoring path, keep the
   framing-validation block intact at the top of the handler.

4. **`UnimplementedVmafxScoringServer` embedding** (`grpc_server.go`):
   the `grpcServer` struct embeds `vmafxv1.UnimplementedVmafxScoringServer`
   so future proto additions don't break the build. Do not remove the
   embed even after every RPC has a real implementation; the generator
   regenerates the unimplemented stub on every proto change.
