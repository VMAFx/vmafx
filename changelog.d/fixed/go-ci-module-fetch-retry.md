- CI: the `go vet + go test` job fetches the Go module graph and installs `gosec` through
  `scripts/ci/go-fetch-retry.sh`, which runs the command at most three times with a growing pause.
  A transient `INTERNAL_ERROR` from the Go module proxy had failed master `a9e3fe2d8` in
  `Install gosec`; `go.sum` and the pinned version still verify every module, so a retry changes no
  result.
