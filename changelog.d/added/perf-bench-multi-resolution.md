### Added multi-resolution performance benchmark baseline

`scripts/perf/bench-multi-resolution.sh` is now the canonical multi-resolution,
multi-backend, multi-metric throughput benchmark for VMAF.  It runs across
[576×324, 640×480, 1080p, 1440p, 2160p] × [cpu, cuda, sycl] × [vif, adm,
motion, ssim, ms_ssim], emits a versioned JSON to
`testdata/perf_multi_resolution.json`, and is used as the reference baseline
for future performance PRs.

Initial baseline generated at master `8930853864` inside `vmaf-dev-mcp:cuda13.3`.

See [ADR-0752](docs/adr/0752-perf-bench-multi-resolution.md) and
[research-0752](docs/research/research-0752-perf-bench-multi-resolution-baseline.md).
