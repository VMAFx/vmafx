### Fixed
- **HIP backend**: registered `test_hip_motion_v2_parity` in `core/test/meson.build`.
  The test was added in PR #913 but was omitted from `core/test/meson.build`,
  leaving the ADR-1106 `integer_motion_v2` mirror fix unverified by an automated
  test target. The test passes on AMD `gfx1036` with bit-exact CPU parity
  (max delta 0.00e+00 across SAD, motion2_v2, and motion3_v2 at `places=4`),
  confirming device parity for all 17 active HIP extractors. Updated
  `core/src/feature/hip/AGENTS.md`, `docs/adr/1154-hip-backend-gaps.md`, and
  `docs/state.md`.
