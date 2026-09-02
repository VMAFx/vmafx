Added name-lookup registration assertions in `core/test/test_metal_smoke.c`
for six Metal feature extractors that previously lacked coverage:
`float_psnr_metal`, `integer_psnr_metal`, `float_ansnr_metal`,
`float_moment_metal`, `float_motion_metal` (with TEMPORAL flag), and
`integer_motion_metal` (with TEMPORAL flag). Also added a registration-coverage
invariant note to `core/src/feature/hip/AGENTS.md` and
`core/src/feature/metal/AGENTS.md` requiring every new backend extractor
to appear in the corresponding smoke test in the same PR.
