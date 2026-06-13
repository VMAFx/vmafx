<!-- markdownlint-disable MD013 MD060 -->
# ADR-0878: Trivy container scan baseline — production images run as non-root

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: @Lusoris
- **Tags**: security, docker, ci, vmafx-rebrand, phase4b

## Context

The fork's published container images
(`ghcr.io/vmafx/vmafx:<tag>` and the GPU variants
`ghcr.io/vmafx/vmafx:<tag>-{cuda12,rocm6,oneapi2026,vulkan}`) are the
canonical user-facing artifact for the Phase 4b cloud-native delivery
mode — k8s pods, the `vmafx-controller` chart, and `docker run` from
the dev quickstart all consume them. Any CVE or hardening miss in those
images is direct user exposure.

A baseline Trivy config scan (v0.69.3) of the three Dockerfiles in tree
flagged the following misconfigurations:

| Dockerfile                        | HIGH | MEDIUM | LOW | Note                                  |
|-----------------------------------|------|--------|-----|---------------------------------------|
| `docker/Dockerfile.production`    | 1    | 0      | 1   | DS-0002 (USER), DS-0026 (HEALTHCHECK) |
| `docker/Dockerfile.production-gpu`| 1    | 0      | 1   | DS-0002 (USER), DS-0026 (HEALTHCHECK) |
| `dev/Containerfile`               | 1    | 6      | 0   | DS-0002 (USER root), 6× DS-0013 (`RUN cd`) |

The HIGH finding on the two production Dockerfiles (DS-0002 — missing
`USER` directive, container runs as root) is the user-impact finding;
the others are either style/DX (`dev/Containerfile` is a development
sandbox where root is intentional) or non-applicable
(`HEALTHCHECK` is the standalone-docker probe mechanism, superseded by
k8s liveness/readiness probes per Phase 4b.9).

Image vulnerability scanning against `ghcr.io/vmafx/vmafx-cpu:latest`
returned `MANIFEST_UNKNOWN` — the production image set has not yet been
published to ghcr (the CI workflow exists per ADR-0698 but has not
fired a tag-triggered build). Image-CVE coverage is therefore a
follow-up that lands the moment the registry has a manifest.

## Decision

Add an explicit `USER nonroot:nonroot` directive (UID 65532, baked into
`gcr.io/distroless/cc-debian12`) to every final stage in both
production Dockerfiles. Leave `dev/Containerfile` as root (intentional;
the image runs `apt-get` and `meson setup` interactively during dev).
Do not add `HEALTHCHECK` — Phase 4b ships k8s probes via the Helm chart
under `deploy/helm/vmafx/templates/deployment.yaml`.

Adopt Trivy config scanning as the standing baseline for new
Dockerfiles: any PR touching `docker/Dockerfile.production*` must
remain HIGH-clean to the `trivy config` gate.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Create a dedicated `appuser` (UID 1000) via `chown` in the builder stage and `COPY --chown` into runtime | Conventional pattern; predictable UID | Distroless `cc-debian12` has no `useradd` / `chown`; the builder stage owns its filesystem but the runtime stage cannot mutate `/etc/passwd` | Distroless already ships `nonroot:nonroot` at 65532 — using the existing user is zero-cost |
| Use `gcr.io/distroless/cc-debian12:nonroot` tag (image where USER is already set) | One-line change | Changes the image tag from a pinned digest to a tag-suffix variant; loses the supply-chain hardening the digest pin provides; would also need the digest re-pinned for `nonroot` | Explicit `USER` directive preserves the digest pin |
| Add `HEALTHCHECK` directive to the server stage | Closes DS-0026 | Standalone-docker probe; k8s ignores it (uses pod spec probes); duplicating the probe logic in two places drifts | k8s probes live in `deploy/helm/vmafx/templates/deployment.yaml` (ADR-0698) — single source of truth |
| Refactor `dev/Containerfile` to drop `USER root` at end | Eliminates the HIGH finding | The dev container exists specifically to provide a root shell for interactive build / debug / `apt-get` workflows; non-root would force `sudo` for every operation | Dev image's role is incompatible with non-root; finding is a false positive in this context |
| Fix the 6× MEDIUM `DS-0013` (`RUN cd …`) findings in `dev/Containerfile` by adding `WORKDIR` between each `RUN` | Cleaner Dockerfile, fewer trivy noise | Mechanical refactor with no security delta (cd inside a single RUN is forked-shell scope, not container scope); adds layer-count without runtime benefit | Style-only; not in scope for a security PR |

## Consequences

- **Positive**: Two HIGH findings cleared. Production images now run as
  UID 65532 — container-escape blast radius reduced to non-root scope.
  CIS Docker Benchmark 4.1 satisfied. Baseline established for future
  Dockerfile work.
- **Negative**: The MCP server stage binds port 8080 (>1024, fine for
  unprivileged users); operators who currently rely on binding <1024
  would need `CAP_NET_BIND_SERVICE` — but no such configuration exists
  in tree. GPU variants require the runtime to inject device nodes with
  group-readable permissions (NVIDIA Container Toolkit does this by
  default; ROCm and oneAPI need `--group-add video,render`).
- **Neutral / follow-ups**:
  - Image-CVE scan blocked on no published `ghcr.io/vmafx/vmafx:*`
    manifest. Re-run `trivy image` when the first production tag fires.
  - Consider wiring `trivy config` into `make lint` (or
    `.github/workflows/security-scans.yml`) so the baseline is
    self-enforcing — separate PR.
  - `dev/Containerfile` keeps its HIGH finding; ADR records the
    rationale so future audits don't re-litigate it.

## References

- Trivy DS-0002: <https://avd.aquasec.com/misconfig/ds-0002>
- Trivy DS-0026: <https://avd.aquasec.com/misconfig/ds-0026>
- CIS Docker Benchmark 4.1 — Ensure that a user for the container has been created
- [ADR-0698](0698-vmafx-production-dockerfile.md): vmafx production Dockerfile (the target this PR hardens)
- [ADR-0709](0709-vmafx-phase4b-distributed-platform.md): Phase 4b cloud-native delivery (containers are the canonical artifact)
- Distroless `cc-debian12` non-root convention: <https://github.com/GoogleContainerTools/distroless/blob/main/base/README.md>
- Source: `req` — task brief 2026-05-30: "Scan published container images for CVEs + misconfigurations via Trivy. ... Per memory `project_vmafx_k8s_cloud_native`, container is the canonical user-facing artifact (Phase 4b.9 decision). CVEs in published images = direct user exposure."
