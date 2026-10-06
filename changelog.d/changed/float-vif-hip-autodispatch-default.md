- **`float_vif_hip` runs for `float_vif` under `--backend hip` by default
  (ADR-2092).** The build option `enable_float_vif_hip_autodispatch` now
  defaults to `true`, so `--backend hip --feature float_vif`, models that
  read `float_vif` and a VMAFx context on a HIP device run the HIP twin, which
  returns the CPU's scores bit for bit (ADR-1444), instead of the CPU
  extractor. Build with `-Denable_float_vif_hip_autodispatch=false` for the
  old behaviour, where the twin runs only as `--feature float_vif_hip`. See
  [`enable_float_vif_hip_autodispatch`](docs/development/build-flags.md#enable_float_vif_hip_autodispatch).
