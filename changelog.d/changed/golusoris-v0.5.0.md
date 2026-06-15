- **Bumped `github.com/golusoris/golusoris` v0.4.0 → v0.5.0.** v0.5.0 carries the
  four fixes vmafx filed and the maintainer integrated (#225 gRPC ServerOption
  injection, #227 operator `ctrl.SetLogger` + webhook config, #234 logger reads
  `log.level` from the prefixed config, plus the version module). This unblocks
  the `vmafx-controller` migration (needs #225) and makes the per-binary interim
  shims (the `VMAFX_LOG_LEVEL` env bridge, the operator SetLogger/webhook-gate
  shims) obsolete — removed in follow-ups. `go build ./...` clean on v0.5.0.
