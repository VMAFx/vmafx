---
paths:
  - core/src/rust/**
  - core/api/vmafx.toml
  - core/src/feature/*.c
  - core/src/libvmaf.c
invariant: New host-side work lands in Rust unless the ADR-2478 phase table says that layer is still C.
---
<!-- markdownlint-disable MD013 MD060 -->
# Rust core migration (ADR-2478)

- New host-side implementation: Rust. Exception: layer still C per ADR-2478 phase table.
- Layer with Rust successor: fix lands in C oracle and Rust, same PR.
- C = differential oracle until layer's C deleted (3.0). Never weaken oracle to pass Rust.
- Layer C builds only in oracle-only Meson profile after phase exit.
- Native device sources + C-only host glue: exception list only: file, rule, reason, trigger, expiry.
- List: `.config/lint-exceptions.d/host-c-cpp.toml` (rule `host-c-cpp`); keys owner, trigger, category beside path/reason/expires. 10 pelorus mirror files on it (owner pelorus, category A, trigger pelorus 2.1 Rust crate). `test_host_c_list_names_every_pelorus_mirror_file` guards it.
- ABI source = `core/api/vmafx.toml`. Never hand-edit exported surface.
- Stable toolchain pinned; nightly only sanitizer lanes + GPU device crates.
- Epic: #2567.
