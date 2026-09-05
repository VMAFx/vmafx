- **Unknown feature options rejected and model GPU twin selection gated.**
  Feature extractor options parsing (`vmaf_fex_ctx_parse_options`) previously
  ignored unrecognised dictionary keys, allowing CLI typos to pass silently and
  causing GPU twins lacking model-requested options (such as `adm_csf_mode: 2`
  in `vmaf_v1.0.16_3d0h`) to run with wrong parameters and emit mismatched feature
  names. Libvmaf now returns `-EINVAL` on unknown options, and
  `vmaf_use_features_from_model` checks GPU twin options against model requirements,
  automatically dispatching extractors with unsupported options to the CPU reference
  twin with an informational notice ([ADR-1183](docs/adr/1183-model-options-gate-gpu-twin-selection.md)).
