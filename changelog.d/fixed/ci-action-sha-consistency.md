Fix corrupted `docker/setup-buildx-action` SHA in `e2e-k8s.yml` (tail differed
from the 7 other identical-version pinnings) and align `actions/checkout` in
`go-ci.yml` and `rust-ci.yml` from v4 to v6, matching the rest of the repo.
