<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1042: Containerfile hardening — non-root USER + build-time DEBIAN_FRONTEND

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `containerfile`, `security`, `docker`, `ci`, `hardening`

## Context

Three container hardening issues were identified in the R8 audit:

**1. `dev/Containerfile` — final stage runs as root**: The last `USER root`
directive (for the pip-install step) was never followed by a `USER vmaf`
before the `ENTRYPOINT`. The entrypoint script and all container processes
ran as uid 0, violating the principle of least privilege.

**2. `Dockerfile.go-server` — no USER directive**: The distroless final stage
had no `USER` instruction. `gcr.io/distroless/cc-debian12` defaults to uid 0
(root) at runtime unless overridden. The distroless image ships a `nonroot`
user (uid 65532) for exactly this purpose.

**3. `Dockerfile` — `ENV DEBIAN_FRONTEND=noninteractive` persists to runtime**:
`ENV` bakes the value into every image layer and all descendant images. This is
only needed during the `apt-get` installation steps; it should not persist into
the final runtime environment. Using `ARG` scopes it to the build context only.

## Decision

1. Add `USER vmaf` after the final `RUN` block and before `ENTRYPOINT` in
   `dev/Containerfile`.
2. Add `USER nonroot` before `ENTRYPOINT` in `Dockerfile.go-server`. The
   distroless `nonroot` user (uid 65532) is always present in
   `gcr.io/distroless/cc-debian12`.
3. Change `ENV DEBIAN_FRONTEND=noninteractive` to
   `ARG DEBIAN_FRONTEND=noninteractive` in `Dockerfile`. The `ARG` value is
   available to all subsequent `RUN` instructions in the build stage but is
   not set in the final image layer.

## Alternatives considered

- **Add a new non-root user in Dockerfile.go-server**: Unnecessary; distroless
  provides `nonroot` (uid 65532) for this purpose.
- **Keep `ENV DEBIAN_FRONTEND`**: Harmless at container runtime (no
  interactive apt sessions), but unnecessarily pollutes the environment of
  every process in the container (including the ffmpeg entrypoint).

## References

- R8 audit: `r8-containerfile-hardening` HIGH — USER root final stage
- R8 audit: `r8-containerfile-hardening` HIGH — no USER in Dockerfile.go-server
- R8 audit: `r8-containerfile-hardening` HIGH — ENV DEBIAN_FRONTEND
- Docker best practices: `ARG` vs `ENV` for build-time variables
