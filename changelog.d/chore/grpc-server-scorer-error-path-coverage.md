### Chore

- Add `TestGRPCScore_ScorerError` to `cmd/vmafx-server/grpc_server_handler_test.go`:
  covers the scorer-failure → `codes.Internal` branch in `grpcServer.Score`
  (grpc_server.go:74–78), which was previously exercised only for the HTTP path
  and the REST adapter. Add `TestRunHTTP_BadAddress` to `main_extra_test.go`:
  covers the `runHTTP` listen-error branch, mirroring the existing
  `TestRunGRPCWithServer_BadAddress` for the gRPC listener.
