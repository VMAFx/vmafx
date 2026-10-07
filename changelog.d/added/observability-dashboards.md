- **Observability: Quality, Nodes and devices, Live sessions and GPU exporter
  dashboards, linted in CI (RC4, ADR-2349, #2430).** Three more dashboards are
  generated from the metric definition: Quality (score levels per model and
  tenant, the bad tail, a score heatmap, the share below 70), Nodes and
  devices (backend per node, slot use, failures and run time per node, GPU
  memory, host memory and CPU) and Live sessions (open ScoreStream sessions,
  frames per second, outcomes, duration); the Overview links to them. One
  dashboard per vendor GPU exporter (NVIDIA dcgm-exporter, AMD
  device-metrics-exporter, Intel XPU Manager) covers utilisation, memory,
  encoder and decoder load and power, to import where that exporter runs.
  `vmafx-node` serves its GPU memory per device (`nvidia-smi` on CUDA nodes,
  the amdgpu sysfs files on HIP nodes); `vmafx-server` and `vmafx-node` serve
  the ScoreStream session families; a failed read of scraped values counts in
  `vmafx_metrics_read_errors_total`. Every dashboard passes Grafana's
  dashboard-linter `--strict` (`make lint-dashboards`, pinned release). See
  [dashboards](docs/development/observability.md#dashboards).
