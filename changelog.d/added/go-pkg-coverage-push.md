## Added

- **Go test coverage push for `pkg/` modules**: focused table-driven tests
  for the lowest-coverage packages (`observability`, `report`, `encoder`,
  `gpu`, `libvmaf`, `storage`, `bisect`). New tests cover Prometheus
  metric registration, controller-source gauge wiring, JSON/Markdown
  report rendering branches, hardware-encoder constructors + QSV env
  override, NVIDIA / AMD / Intel / Apple Metal probe parsers via PATH
  shims, libvmaf path allow-list validation, VMAF XML score parsing,
  storage argv builders + helper edge cases. Per-package coverage moves
  from `observability` 0.0%→96.9%, `report` 0.0%→98.1%, `encoder`
  31.2%→85.1%, `gpu` 30.5%→81.0%, `libvmaf` 38.4%→84.8%, `storage`
  41.9%→55.8%, `bisect` 44.9%→92.8%. No production code touched.
