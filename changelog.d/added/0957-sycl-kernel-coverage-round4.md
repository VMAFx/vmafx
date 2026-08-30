## Added

- **SYCL kernel coverage round 4** (ADR-0957,
  `core/test/test_sycl_float_moment_parity.c`,
  `core/test/test_sycl_speed_chroma_parity.c`,
  `core/test/test_sycl_speed_temporal_parity.c`,
  `core/test/test_sycl_ssimulacra2_parity.c`): four new CPU vs.
  SYCL parity gates closing the last uncovered SYCL extractors after
  round 3 (#446). `float_moment_sycl`, `speed_chroma_sycl`, and
  `speed_temporal_sycl` are gated at ADR-0214 places=4 (1e-4)
  tolerance; `ssimulacra2_sycl` uses 5e-3 to match the ADR-0214
  `FEATURE_TOLERANCE` entry for its multi-stage XYB + IIR +
  SSIM-combine pipeline. Closes the SYCL kernel-coverage backlog
  enumerated in ADR-0946 (4 of 4 round-4 targets covered). Each
  test mirrors the round-3 scaffold: 256x144 synthetic YUV420P
  fixture, public `vmaf_use_feature` API, skip-on-no-device path.
  The `core/src/feature/sycl/AGENTS.md` coverage matrix is closed
  (no remaining round-N backlog rows). Discovered + documented
  during this work: the 752-LOC `speed_chroma_sycl.cpp` and
  705-LOC `speed_temporal_sycl.cpp` source files exist on disk but
  are not yet wired into `sycl_feature_sources` in
  `core/src/meson.build` or the extractor registry in
  `core/src/feature/feature_extractor.c`; their parity tests ship
  in dormant form (`[skip: <name> not built into libvmaf]`) and
  auto-activate as real gates once a follow-up PR wires the TUs
  into the build + registry.
