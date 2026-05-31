- **Docs**: Added a standalone Doxyfile for the libvmaf public C API
  (`core/doc/Doxyfile.public-api`) and an on-demand
  `doxygen-public-api` CI workflow that publishes the generated HTML
  and warning log as build artifacts. Drove the baseline warning
  count from **95 → 0** on doxygen 1.15 by documenting every
  previously-undocumented struct member, function parameter, and
  return value across `core/include/libvmaf/*.h`; removed the
  unsupported `@field` antipattern from `libvmaf_mcp.h`, `dnn.h`,
  and `libvmaf_metal.h`; and replaced cross-symbol `@ref` from
  struct doc-blocks with backtick literals where doxygen could not
  resolve them. The workflow is informational only; promotion to a
  required gate is tracked in ADR-0953.
