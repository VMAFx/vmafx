- Corrected the `integer_motion_v2.metal` header comment to document the reflect-101
  mirror fold implemented in #1223, closed out the deferred Metal mirror status
  from ADR-1166 via ADR-1176, updated `test_metal_motion_v2_parity` to return exit code
  77 (meson skip) on `-ENODEV` and log active device execution to stdout for CI observability,
  and updated `docs/state.md`.
