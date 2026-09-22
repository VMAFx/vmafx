- C unit tests under `core/test/` now satisfy the repository's own
  HISS-21 invariants: 51 findings cleared without a baseline edit, a
  suppression, or a weakened assertion. Two unbounded read loops
  (`test_vmaf_tiny_v2.py`, `test_vmaf_use_tiny_model.c`) gained a
  scalar bound derived from the file size, with bound exhaustion
  reported as an error; four `goto` cleanup jumps in
  `test_float_adm_dwt2_neon.c` became one unconditional buffer-release
  path; and 45 over-long bodies were split into cohesive helpers. Every
  assertion string, expected value and test registration is unchanged,
  and the three CUDA picture-preallocation cases plus twelve HIP parity
  scaffold-skip sites now share one implementation each
  (`run_preallocation_method()`, `core/test/hip_parity_skip.h`).
  Two bodies are deliberately left intact, each behind the cited
  suppression that already protects it: `ref_calc_psnrhvs()` in
  `test_psnr_hvs_simd.c`, whose structure is the ADR-0138 bit-exactness
  contract, and `test_barten_csf()` in `test_barten_csf.c`, whose case
  list is carried verbatim from Netflix upstream so later syncs of that
  file stay a diff-and-merge. The clang-tidy ratchet records no
  regression on any touched file, and the four files that came out below
  their allowance (`test_dict.cpp` 33 -> 31,
  `test_pic_preallocation.c` 16 -> 10, `framesync.c` 7 -> 0,
  `test_framesync.c` 8 -> 0) tighten
  `scripts/ci/tidy-baseline-cpu.json` through the scoped writer rather
  than by hand; see
  `docs/research/core-test-hiss21-burndown-2026-09-21.md`.
