- CI: the `Docker Image Build work` job may run for 90 minutes instead of 45. The uncached
  image build took 30 to 42 minutes on the nine full `master` builds of 2026-10-10 and
  2026-10-11, and the build of `0d2ca56d0` was ended by the limit while it was still
  building, which turned `master` red (`.github/workflows/docker-image.yml`).
