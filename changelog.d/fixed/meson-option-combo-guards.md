### Meson build-option combination validation

**Fixed:** Three broken build-option combinations now produce clear diagnostics instead of silently succeeding with no effect (audit findings 1b, 1c, 1d from audit-build-matrix-symbols-2026-05-16).

- `enable_mcp_sse=enabled` + `enable_mcp=false` now errors: "enable_mcp_sse=enabled/true requires enable_mcp=true"
- `enable_mcp_uds=true` + `enable_mcp=false` now errors: "enable_mcp_uds=true requires enable_mcp=true"
- `enable_mcp_stdio=true` + `enable_mcp=false` now errors: "enable_mcp_stdio=true requires enable_mcp=true"
- `enable_avx512=true` + `enable_asm=false` now warns: "enable_avx512=true has no effect when enable_asm=false"
- `enable_hipcc=true` + `enable_hip=false` now warns: "enable_hipcc=true has no effect when enable_hip=false"

The validation runs at configuration time in `core/src/meson.build` before any subdirectory inclusions, ensuring users see the problem immediately.

**Also:** bumped `meson_version` constraint from `>= 0.56.1` to `>= 0.58.0` in `core/meson.build` because `core/src/vulkan/meson.build:118` uses `str.replace()`, which requires meson 0.58.0.
