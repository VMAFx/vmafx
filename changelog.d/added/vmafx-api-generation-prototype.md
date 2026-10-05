- **Preview of the VMAFx C API, generated from one definition (RC4,
  ADR-1852).** New headers `vmafx/vmafx.h` and `vmafx/libvmaf_bridge.h` with
  `vmafx_context_create` / `vmafx_context_destroy`, version, provenance,
  extractor and feature-score queries and errors that name what failed; a
  standard-library Python binding (`bindings/python/vmafx/`); and
  `scripts/codegen/vmafx-api.py`, which generates the headers, the binding,
  the ABI layout test, the reference page and the `libvmaf.h` shims for
  `vmaf_init`, `vmaf_close`, `vmaf_version` and `vmaf_feature_score_at_index`
  from `core/api/vmafx.toml`. `libvmaf.h` behaviour is unchanged. ABI 0.1 is a
  preview until `v1.0.0`. See [the VMAFx API page](docs/api/vmafx/index.md)
  and [API generation](docs/development/api-generation.md).
