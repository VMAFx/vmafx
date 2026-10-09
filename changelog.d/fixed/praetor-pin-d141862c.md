- **The `Go API Compatibility` check compares Go APIs again on pull requests
  ([ADR-3061](docs/adr/3061-praetor-pin-d141862c.md)).** Once setup-go's `stable` resolved to Go
  1.27.2, the check failed on every pull request with "the checker reported the canary's removed
  exported function as compatible". Go 1.27.2 writes export data that the checker's
  `golang.org/x/tools` v0.49.0 cannot read. The praetor pin moves from `3a766f2d56ad` to
  `d141862c430b`, whose API gate builds go-apidiff in a pinned build module with x/tools v0.51.0.
