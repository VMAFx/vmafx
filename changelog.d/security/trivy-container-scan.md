- Production container images (`docker/Dockerfile.production` and
  `docker/Dockerfile.production-gpu`) now drop privileges to
  `nonroot:nonroot` (UID 65532, baked into `gcr.io/distroless/cc-debian12`)
  in every final stage. Clears two Trivy DS-0002 HIGH findings on the
  fork's canonical user-facing artifact. The MCP server stage continues
  to bind 8080 (unprivileged port). GPU variants (cuda12 / rocm6 /
  oneapi2026 / vulkan) drop the same way; operators inject device nodes
  via the NVIDIA Container Toolkit (CUDA) or `--group-add video,render`
  (ROCm / oneAPI). `dev/Containerfile` keeps `USER root` — intentional
  dev sandbox. See ADR-0878.
