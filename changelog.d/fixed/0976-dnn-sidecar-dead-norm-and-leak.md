- `core/src/dnn/`: removed the dead `has_norm` / `norm_mean` /
  `norm_std` / `expected_min` / `expected_max` / `has_range` fields
  from `VmafModelSidecar` and the three consumer branches in
  `dnn_api.c` / `libvmaf.c` that read them — the JSON parser never
  populated these fields, so the consumers always selected
  mean=NULL / std=NULL anyway. Per
  [ADR-0114](../docs/adr/0114-coverage-gate-per-file-overrides.md)
  Alternatives §2 deferred cleanup. Same commit plugs a real leak
  in `extract_string_array`: malformed sidecar JSON (`-EINVAL`,
  `-ERANGE`, `-ENOMEM` paths) previously left partially-allocated
  strings in the caller's `out[]` while leaving `*out_n` unwritten,
  so the caller's fallback cleanup loop iterated zero times and the
  strings leaked. Producer now owns its own cleanup on every error
  return. Two regression tests added; verified failing without the
  fix. See [ADR-0976](../docs/adr/0976-dnn-sidecar-dead-norm-fields-removal.md).
