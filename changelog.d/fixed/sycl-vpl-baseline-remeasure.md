- Remeasured `core/tools/vmaf_vpl.c` through the official SYCL lane and generator tooling,
  updating `scripts/ci/tidy-baseline-sycl.json` to reflect the reduced warning count (21 -> 12)
  following HISS-21 function size decomposition and nullptr modernization (ADR-1243).
