- Remove duplicate `speed_chroma_hip.c` / `speed_temporal_hip.c` entries in
  `core/src/hip/meson.build`. ADR-0852 added a second wiring block for these
  two TUs without noticing that ADR-0964 had already included them earlier in
  the same `hip_sources` list. The duplication caused duplicate-symbol link
  errors when building with `enable_hip=true` and `enable_hipcc=true`.
