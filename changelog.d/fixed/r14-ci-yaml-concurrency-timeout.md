Added missing `concurrency` blocks (with `cancel-in-progress`) and `timeout-minutes`
to 16 GitHub Actions workflow jobs that were left without these guards. Affected
workflows: `docker-image`, `docker-publish-production`, `docs`, `ffmpeg-integration`
(3 jobs), `go-ci`, `libvmaf-build-matrix`, `nightly`, `nightly-bisect`,
`release-please`, `rust-ci`, `scorecard`, `supply-chain`, `upstream-watcher`,
`upstream-ffmpeg-hip-hwdec-watcher`, `upstream-netflix-645-hdr-model-watcher`,
`upstream-netflix-955-watcher`.
