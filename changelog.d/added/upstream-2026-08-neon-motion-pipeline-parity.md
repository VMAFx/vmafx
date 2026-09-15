- **The arm64 integer-motion pipelines now have bit-exactness coverage**
  (adapted from upstream Netflix/vmaf `3137d5525`). That upstream commit adds an
  8-bit NEON motion pipeline this fork already had — `motion_v2_neon.c` has
  exported `motion_score_pipeline_8_neon` **and** a 16-bit twin upstream still
  lacks, both dispatched from `integer_motion.c` under
  `VMAF_ARM_CPU_FLAG_NEON` — so its implementation was not taken. Its *test* was
  the part missing here: the header claimed bit-exactness against the scalar
  references and nothing asserted it, while the sibling `test_motion_neon.c`
  covers only `x_convolution_16_neon`, one kernel inside the 16-bit pipeline.
  New `core/test/test_motion_pipeline_neon.c` drives the registered `motion`
  extractor twice over identical pictures, once with the CPU-flag mask cleared
  and once with NEON enabled, and requires the pooled SAD to be equal
  bit-for-bit across 15 geometries, three bit depths (8, 10 and 12, so both
  pipelines are covered) and three seeds. Verified by cross-compiling for
  aarch64 and executing under emulation, and confirmed to have teeth: changing
  one filter tap from 16004 to 16005 makes it fail.
