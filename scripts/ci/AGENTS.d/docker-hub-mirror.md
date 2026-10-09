---
paths:
  - scripts/ci/docker-hub-mirror.sh
  - scripts/ci/tests/test_docker_hub_mirror.py
invariant: Jobs pulling docker.io images run docker-hub-mirror.sh first; image references keep their Docker Hub digests.
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
  `--print` mode is root-free merge the test checks.
