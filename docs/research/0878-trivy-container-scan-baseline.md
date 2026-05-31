<!-- markdownlint-disable MD060 -->
# Research digest — Trivy container scan baseline (2026-05-30)

ADR: [ADR-0878](../adr/0878-trivy-container-scan-baseline.md)
PR: `chore/trivy-container-scan`
Tool: `trivy v0.69.3` (config scan + image scan attempt)
Targets: `docker/Dockerfile.production`,
`docker/Dockerfile.production-gpu`, `dev/Containerfile`,
`ghcr.io/vmafx/vmafx-cpu:latest` (image scan)

## TL;DR

Three Dockerfiles scanned. Two HIGH findings on the user-facing
production images (DS-0002 — running as root) fixed via
`USER nonroot:nonroot` directive on every final stage. Dev Containerfile
keeps its HIGH (intentional). Image-CVE scan blocked on
`MANIFEST_UNKNOWN` — production images not yet published to
`ghcr.io/vmafx/vmafx:*`.

## Config scan results

### Before

| File                              | HIGH | MEDIUM | LOW | Status                |
|-----------------------------------|------|--------|-----|-----------------------|
| `docker/Dockerfile.production`    | 1    | 0      | 1   | RED                   |
| `docker/Dockerfile.production-gpu`| 1    | 0      | 1   | RED                   |
| `dev/Containerfile`               | 1    | 6      | 0   | RED (intentional)     |

Finding breakdown:

- **DS-0002 (HIGH)** — `docker/Dockerfile.production`: no `USER` directive
  on `cli` or `server` final stages. Container runs as root (UID 0). The
  distroless base shell-less, so the blast radius is bounded — but the
  process still inherits root file permissions on bind-mounted host
  volumes, which is the realistic exposure for `docker run -v
  /host/path:/mount vmafx:cpu vmaf ...`.
- **DS-0002 (HIGH)** — `docker/Dockerfile.production-gpu`: same finding,
  applies to all five final stages (`final-cpu`, `final-cuda12`,
  `final-rocm6`, `final-oneapi2026`, `final-vulkan`).
- **DS-0002 (HIGH)** — `dev/Containerfile:902`: last `USER root`
  intentional. The dev container runs `apt-get`, `meson setup`, and
  `pip install` interactively. False positive in this context; ADR
  records the rationale.
- **DS-0013 (MEDIUM) × 6** — `dev/Containerfile`: `RUN cd /path && ...`
  patterns at lines 234, 275, 533, 549, 863, and one more. Style finding;
  scope of the `cd` is the shell-fork, not the container WORKDIR, so
  there's no functional difference. Not in scope.
- **DS-0026 (LOW)** — both production Dockerfiles: no `HEALTHCHECK`.
  Phase 4b ships k8s liveness/readiness probes via the Helm chart at
  `deploy/helm/vmafx/templates/deployment.yaml` (see ADR-0698 § probes).
  Standalone-docker probe would duplicate logic.

### After

Production Dockerfiles re-scanned post-fix:

```text
$ trivy config --skip-version-check docker/Dockerfile.production
Tests: 27 (SUCCESSES: 26, FAILURES: 1)
Failures: 1 (UNKNOWN: 0, LOW: 1, MEDIUM: 0, HIGH: 0, CRITICAL: 0)
DS-0026 (LOW): Add HEALTHCHECK instruction in your Dockerfile
```

```text
$ trivy config --skip-version-check docker/Dockerfile.production-gpu
Tests: 27 (SUCCESSES: 26, FAILURES: 1)
Failures: 1 (UNKNOWN: 0, LOW: 1, MEDIUM: 0, HIGH: 0, CRITICAL: 0)
DS-0026 (LOW): Add HEALTHCHECK instruction in your Dockerfile
```

Both HIGH cleared. The remaining LOW is by design.

## Image-CVE scan attempt

```text
$ trivy image --skip-version-check --severity HIGH,CRITICAL \
    ghcr.io/vmafx/vmafx-cpu:latest
FATAL: ... GET https://ghcr.io/v2/vmafx/vmafx-cpu/manifests/latest:
       MANIFEST_UNKNOWN: manifest unknown
```

The image set described in ADR-0698 is not yet published. The CI
workflow scaffold exists but no tag has fired a build. Image-CVE
coverage is therefore a follow-up the moment the registry has a
manifest. Suggested workflow:

```yaml
# in .github/workflows/security-scans.yml (separate PR)
- name: trivy-image-scan
  if: github.event_name == 'release' || github.ref == 'refs/heads/master'
  uses: aquasecurity/trivy-action@master
  with:
    image-ref: ghcr.io/vmafx/vmafx:${{ github.ref_name }}
    severity: HIGH,CRITICAL
    exit-code: 1
```

## Why `USER nonroot:nonroot` is the right answer

`gcr.io/distroless/cc-debian12` is pinned by digest in both production
Dockerfiles. The image bakes in two non-root identities:

| User      | UID:GID     | Shell | Home          | Use                       |
|-----------|-------------|-------|---------------|---------------------------|
| `root`    | 0:0         | none  | `/root`       | Default; not desired here |
| `nonroot` | 65532:65532 | none  | `/home/nonroot` | The drop-target           |

No `useradd` / `chown` is needed — distroless already has the user.
The runtime stages don't write to disk (read-only model dir, no temp
files), so no further `chown` adjustments are required.

The `:nonroot` tag-suffix variant of distroless would have set the
USER for us, but switching from a digest pin to a tag would lose the
supply-chain hardening; explicit `USER` directive is the cleaner path.

## Reproducer

```bash
# In a fresh worktree on this branch
trivy config --skip-version-check docker/Dockerfile.production
trivy config --skip-version-check docker/Dockerfile.production-gpu
trivy config --skip-version-check dev/Containerfile

# Expected post-fix:
#   production:     1 LOW (DS-0026), 0 HIGH
#   production-gpu: 1 LOW (DS-0026), 0 HIGH
#   dev:            6 MEDIUM + 1 HIGH (all intentional)
```

## Follow-ups

1. Publish `ghcr.io/vmafx/vmafx:*` images via CI tag-trigger so Trivy
   image-CVE scans can run.
2. Wire `trivy config` into either `make lint` or
   `.github/workflows/security-scans.yml` as a self-enforcing gate.
3. Consider an `appuser` (UID 1000) for GPU variants if device-node
   permissions become friction — but only after the first user reports
   trouble; current GID 65532 works with NVIDIA Container Toolkit.

## Source

`req` — task brief 2026-05-30: "Scan published container images for CVEs

- misconfigurations via Trivy. ... CVEs in published images = direct
user exposure."
