- CI: the `Dev Container Build work` job runs on `ubuntu-26.04` again. A new
  step, `scripts/ci/free-runner-disk.sh`, removes the preinstalled SDKs no step
  uses before the image build, which ran out of disk on that image. The two
  FFmpeg build jobs keep their `ubuntu-24.04` fallback until 2026-10-29: no
  release of the shared FFmpeg fix series removes the compiler warnings yet
  ([CI runners](docs/development/ci-runners.md)).
