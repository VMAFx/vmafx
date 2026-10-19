- **Rust integer ADM extractor (`adm_rust`)**: builds configured with
  `-Denable_rust_features=true` contain a Rust port of the fixed-point `adm`
  extractor. It takes every option of `adm` and returns its `adm2`, `aim`,
  `adm3`, scale and debug scores bit for bit; select it with
  `VMAF_FEATURE_IMPL=rust` or `--feature adm_rust`
  ([ADM](docs/metrics/adm.md#rust-implementation-adm_rust), #1723, ADR-1713).
