- Release containers now keep their build and runtime ABIs aligned: CPU and
  node images use Debian 13 end to end, the MCP server carries its matching
  Python 3.14 interpreter, GPU stages use digest-pinned CUDA 13.3.1, ROCm
  7.2.4, and oneAPI 2025.3.1 vendor images, and arm64 node builds collect
  native FFmpeg dependencies instead of copying x86-only paths. Publishing a
  GitHub release now triggers both Docker workflows at the release tag; strict,
  signature-verifying smokes replace success-masked checks. The Go scoring
  server, operator, and node images inject that release tag into the shared Go
  version package and expose a non-blocking `--version` check for runtime
  verification; the Go server is now published as a signed multi-architecture
  `ghcr.io/vmafx/vmafx-server:v3.2.1` image with live health/readiness probes.
- The node keeps its established `:50052` standalone gRPC default while
  preserving explicit file and environment overrides. Its image now exports
  the consumed `VMAFX_MODEL_DIR` variable, and its node documentation no
  longer promises unavailable hardware encoders, backend auto-detection, or a
  nonexistent Prometheus listener.
- The Python MCP release server now registers tools through the MCP 2.1
  constructor API, preserves `isError` tool failures and progress sessions,
  restores its documented `--transport http` dispatch, and installs the
  required HTTP dependencies in the production image.
- Manual Docker recovery now refuses to publish an input tag from unrelated
  branch source. Both image workflows require the same existing published
  ordinary tag in their input, workflow ref, source SHA, checkout, and
  coordinated version files before any package write or OIDC operation.
