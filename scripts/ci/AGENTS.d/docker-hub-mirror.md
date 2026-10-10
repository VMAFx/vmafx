---
paths:
  - scripts/ci/docker-hub-mirror.sh
  - scripts/ci/tests/test_docker_hub_mirror.py
  - .github/workflows/security-scans.yml
  - .github/workflows/reuse.yml
  - .github/workflows/docker-image.yml
  - .github/workflows/dev-container-build.yml
invariant: Steps pulling docker.io run docker-hub-mirror.sh first; job-setup pulls name mirror.gcr.io; digests unchanged.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Docker Hub pull-through mirror

- `go vet + go test` (`.github/workflows/go-ci.yml`) runs
  `docker-hub-mirror.sh` before `go test`: daemon pulls `docker.io` through
  `mirror.gcr.io`, so testcontainers PostgreSQL pulls of `storetest` stop
  hitting Docker Hub anonymous limit. Pattern = golusoris #741.
- Mirror lives in daemon, not in image strings: `storetest.Image` and
  `OldestImage` keep `postgres:<tag>@sha256:<digest>` (Renovate rule in
  `renovate.json` matches `postgres`). Mirror serves same digests.
- Script fails when `docker info` lacks mirror; never add retry loop instead.
  `--print` mode = root-free merge under test.
- Step pulls: `Docker Image Build` (`docker-image.yml`) and
  `Dev Container Build work` (`dev-container-build.yml`, incl.
  `rocm/dev-ubuntu-26.04`) run script before build; BuildKit takes
  daemon mirrors for docker.io, Docker Hub = fallback.
- Job-setup pulls (job `container`, `services`, `uses: docker://`) happen
  before any step -> daemon mirror never reaches those pulls. Reference names
  `mirror.gcr.io/<repo>@<same digest>` directly: semgrep container
  (`security-scans.yml`), reuse step (`reuse.yml`, formerly Dockerfile action
  `fsfe/reuse-action`). `WorkflowImages` in test refuses Docker Hub reference
  there; `${{ }}` image = not checked.
- Known gap: Dockerfile-based actions build at job setup
  (`EmbarkStudios/cargo-deny-action`, `FROM rust:...-alpine`); no redirect
  short of replacing action. Not covered by guard.
