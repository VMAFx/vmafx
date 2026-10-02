- **Ten CPU feature extractor and support files conform to clang-tidy standards (batch 3).**
  The third batch of CPU feature extractors and support headers (`alias.c`,
  `feature_name.h`, `iqa/iqa.h`, `iqa/iqa_options.h`, `iqa/iqa_os.h`,
  `moment_options.h`, `motion_blend_tools.h`, `motion_options.h`, `motion_tools.h`,
  and `niqe_model.h`) were refactored to zero clang-tidy findings in the CPU lane
  under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). All 12 reference
  score configurations remain bit-identical at `--precision max` across scalar,
  AVX2, and AVX-512 cpumasks, and the Netflix CPU golden gate passes with unchanged
  counts.
