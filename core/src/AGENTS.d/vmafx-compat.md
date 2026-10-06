---
paths:
  - core/src/compat/libvmaf/*
  - core/src/vmafx/engine_names_gen.h
  - core/src/vmafx/bridge.c
  - core/src/vmafx/context_frames.c
  - core/src/vmafx/convert.c
  - core/src/vmafx/tiny_model.c
  - core/src/vmafx/mcp_server.c
  - core/src/vmafx_legacy_*.map
  - core/src/libvmaf_symbols.txt
  - core/test/test_compat_*
  - core/test/compat_conformance*
invariant: libvmaf.so.3 links exported VMAFx symbols only; engine TUs need engine_names_gen.h; GPU exceptions declared.
---
<!-- markdownlint-disable MD013 -->
# libvmaf compat library on libvmafx (ADR-1852 D3, ADR-2094)

- `libvmafx.so.1` = engine + VMAFx API. `libvmaf.so.3` = `core/src/compat/libvmaf/` only, `link_with` libvmafx of same kind (shared + static built separately: both_libraries would link static compat to shared engine). Compat shared link uses Meson `b_lundef` (`-Wl,--no-undefined`): compat code reaching non-exported engine symbol = link error (`test_compat_library_gates` probe). Compat code calls exported vmafx_ only; status->errno via `vmaf_compat_status_errno()` (generated copy, `status_errno_gen.c`), never `vmafx_status_to_errno()` (hidden).
- Engine names: `core/src/vmafx/engine_names_gen.h` forced on every C/C++ TU (`add_project_arguments(vmaf_engine_name_args)`, `-include` / `/FI`; Obj-C++ via `metal_objcpp_args`): `#define vmaf_<stem> vmaf_engine_<stem>` for every compat entry except kind `engine`. Engine sources keep libvmaf names in source; upstream sync ports unchanged. Never compile an engine TU without it (static link would bind libvmaf call to engine or recurse through shim). Compat lib, tools, black-box tests (`libvmaf_public_link`) add `vmaf_public_name_args` (`-DVMAF_PUBLIC_NAMES`); C++ black-box tests need it in `cpp_args` too. Header `#error`s without switches `VMAFX_ENGINE_EXPORTS_{CUDA,SYCL,HIP,METAL}`, `VMAFX_BUILD_MCP`.
- Test links: white-box `vmaf_test_link` (static compat carrying static engine; engine names visible), black-box `libvmaf_public_libs`. Never `libvmaf.get_static_lib()` / `get_shared_lib()`: compat targets are plain `shared_library` / `static_library` (`libvmaf_shared_lib`, `libvmaf_static_lib`, `libvmafx_shared_lib`, `libvmafx_static_lib`).
- `[[compat]]` kinds: `shim` / `glue` generated (`libvmaf_gen.c`), `manual` (`file` under `core/src/compat/libvmaf/`, `calls` = vmafx_ functions used), `engine` (no compat definition; engine keeps + exports it while `engine_with` backend built; `until` = what ends it). `engine_with` on manual = compat definition only without that backend (HIP / Metal absent-backend files). `when = "mcp"` = only builds with MCP. Exceptions now: CUDA (5, until #2277 lane), SYCL (20), HIP (5), Metal (9) while built.
- Conformance: `test_compat_conformance` runs each scenario through old bodies (table compiled with engine names) and compat (table with public names); traces (rc, outputs, scores `%a`) must be equal; every compat function of this build called through both tables (coverage). New compat function = new scenario call, else red. Planted: `VMAF_COMPAT_PLANT=score|uncovered` (should_fail tests). Report traces skip `fps`, `provenance`, `feature_backends`, `backend_used` (additive members of VMAFx contexts) and JSON trailing commas.
- Rebase onto master: master has `vmaf_set_sample_range_check_enabled` (#2221), #2300 adds `vmaf_set_input_colorimetry` (ADR-2093): 109 functions. `test_libvmaf_deprecation` red until each has `[[compat]]` entry + marker; conformance coverage red until a scenario calls it. Colorimetry plan (ADR-2094 follow-ups): VMAFx per-frame colour (`VmafxFrameDesc` appended `VmafxColor`) + context default colour; compat calls the context function; submit feeds engine conversion state; change after first converted pair = `VMAFX_E_BUSY`.
- Known deliberate differences (docs/api/vmafx/index.md): `vmaf_write_output` unknown format leaves file untouched; override for extractor model does not read: success, not in provenance; `vmaf_model_feature_overload` on lead still held by its collection = `-EBUSY` (lent-ref trick only in collection overload); model overload while mounted = `-EBUSY`.
