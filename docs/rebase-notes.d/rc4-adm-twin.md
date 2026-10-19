## Rust integer ADM twin (`core/src/rust/feature/adm/`, RC4) (2026-10-07)

`rc4/adm-twin`, [ADR-1713](adr/1713-rc4-rust-extractor-framework.md).

- The crate `vmafx-fex-adm` ports the scalar path of
  `core/src/feature/integer_adm.c`, `integer_adm.h`, `integer_adm_kernels.h`,
  `adm_csf_fixed_point.h`, `adm_cm_accumulator.h`, `adm_angle_flag.h`,
  `adm_score.h` and `barten_csf_tools.h` statement by statement and registers
  it as `adm_rust` (ADR-1713). An upstream sync or rebase that changes the
  arithmetic, the option table or the emitted names of `adm` changes the crate
  in the same change and re-runs `scripts/ci/rust_twin_diff.py --feature adm`
  on every fixture (`core/src/feature/AGENTS.d/adm-rust-twin.md`). A change to
  a float table of `integer_adm.h` or `barten_csf_tools.h` also regenerates
  `core/src/rust/feature/adm/src/tables_c.rs`. No score, public C API or
  FFmpeg patch impact.
