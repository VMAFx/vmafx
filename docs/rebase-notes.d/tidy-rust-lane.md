## Tidy lane `rust` for the sources built with `enable_rust_features` (2026-10-27)

- `scripts/dev/tidy-lane.sh`: the `rust` lane installs the toolchain pinned by
  `RUST_VERSION` in `build-config.env` under `/opt/vmafx-rust`. Fork-only.
- A new C or C++ unit built only with `-Denable_rust_features=true` needs its
  path in `TIDY_RATCHET_EXTRA_rust` in the `Makefile`, or the coverage check fails.
- No upstream file is touched; nothing to merge from Netflix/vmaf.
