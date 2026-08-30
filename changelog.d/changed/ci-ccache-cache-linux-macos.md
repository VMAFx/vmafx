- **CI:** `actions/cache` now persists `~/.ccache` for every Linux + macOS
  build leg in `libvmaf-build-matrix.yml` (previously only the MinGW64 leg
  cached its `.ccache`). After warm-up, ccache hit rate of 60–85% is expected
  per Research-0089 §3.1, dropping the critical-path build wall-clock by
  ~3–5 min/cell (~4 min PR-end-to-end, ~50 runner-min/PR). No coverage
  change — `ccache -s` is logged after every build so the warm-up curve is
  visible in CI. See `docs/research/0089-ci-cost-optimization-audit-2026-05-09.md`.
