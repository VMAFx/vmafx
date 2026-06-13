- T7-5 — NOLINT-sweep closeout (ADR-0278). Cite-only pass that adds
  explicit `(ADR-0141 §2 ... load-bearing invariant; T7-5 sweep
  closeout — ADR-0278)` references to the 22 surviving
  `readability-function-size` NOLINT sites in `core/src/` +
  `core/tools/` whose comments described the invariant in prose
  without naming an ADR explicitly. Touches `integer_adm.c`
  (1 site, upstream-mirror Netflix `966be8d5`),
  `cuda/ssimulacra2_cuda.c` (3 sites),
  `vulkan/ssimulacra2_vulkan.c` (3), `vulkan/cambi_vulkan.c` (1),
  `sycl/integer_adm_sycl.cpp` (6), `sycl/integer_motion_sycl.cpp` (2),
  `sycl/integer_vif_sycl.cpp` (4), `tools/vmaf.c` (3 driver
  functions). After this PR, programmatic audit reports 75 sites
  total, 0 missing ADR/Research citations. No function bodies
  split, no behavioural change, no Netflix golden assertion touched.
  Companion research digest at
  [`docs/research/0063-t7-5-nolint-sweep.md`](docs/research/0063-t7-5-nolint-sweep.md).
  Closes backlog item T7-5.
