- **fix(rust):** Add `Default` implementation for `VmafContext` in `vmafx-sys/safe.rs`;
  re-trigger Rust CI against fixed C library build (ADR-0994 fixed `integer_motion.c`
  `prev_prev_ref` compile error that had blocked the Rust CI job since `83cbcbef7f`).
