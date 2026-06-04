- `dev/Containerfile`: add `USER vmaf` after the final pip-install `RUN` block
  and before `ENTRYPOINT`; the container no longer runs as root at runtime
  (ADR-1042).
- `Dockerfile.go-server`: add `USER nonroot` (uid 65532) before `ENTRYPOINT`
  in the distroless final stage; eliminates root-at-runtime for the Go server
  image (ADR-1042).
- `Dockerfile`: change `ENV DEBIAN_FRONTEND=noninteractive` to `ARG` so the
  value is scoped to the build context only and does not persist into the
  runtime image layer (ADR-1042).
