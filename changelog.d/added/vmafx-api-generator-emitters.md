- **VMAFx API generator: header split, symbol versions and ABI gates (RC4,
  ADR-1852).** The VMAFx API headers follow the design's layout:
  `vmafx/vmafx.h` includes `version.h`, `types.h`, `error.h`, `context.h`,
  `device.h`, `frame.h`, `model.h`, `score.h`, `provenance.h`, `report.h`,
  `dnn.h` and `mcp.h`, each usable on its own; `vmafx/libvmaf_bridge.h` stays
  optional. On Linux every `vmafx_*` symbol carries the version node of the ABI
  minor that introduced it (`VMAFX_0.1`). The definition format
  (`core/api/vmafx.toml`) gains header groups, callbacks, flag sets, fixed
  arrays, nested sized structs, per-entry `since` and `deprecated`, and option
  groups; `scripts/codegen/vmafx-api.py` also writes the linker version
  script, the Windows export list, the exported-symbol list, the header
  install list, one reference page per header, and a changelog draft
  (`--changelog <ref>`). New Meson tests check the definition is append-only
  against the merge base and run the generator's own tests. See
  [API generation](docs/development/api-generation.md).
