- **MCP subprocess cancellation** (`cmd/vmafx-mcp/impl.go`): three
  `exec.Command` (no context) calls replaced with `exec.CommandContext(ctx,
  ...)` so MCP client disconnection / timeout terminates the vmaf scoring
  subprocess, Python ONNX evaluator, and GPU backend probe — rather than
  letting them run to completion indefinitely (ADR-1018, r5-mcp-streaming
  findings :224, :527, :788).
- **Controller gRPC panic recovery** (`cmd/vmafx-controller/grpc_server.go`):
  `recoveryUnaryInterceptor` and `recoveryStreamInterceptor` added to the
  controller's `grpc.NewServer` options, mirroring the vmafx-server pattern
  (ADR-0978). A panic in any controller handler (SubmitJob, GetJob, CancelJob,
  PullWork, ReportResult, StreamJobs) is now caught and returned as
  `codes.Internal` instead of crashing the process (ADR-1018,
  r5-grpc-correctness finding grpc_server.go:418).
