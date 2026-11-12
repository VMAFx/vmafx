<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# VMAFx API, frames, and provenance: `core/src/` agent invariants

vmafx/*, compat/libvmaf/*, core/api/vmafx.toml, core/include/vmafx/*,
vmafx*.map, vmafx.def, libvmaf_symbols.txt, device frame import, live windows,
and provenance records.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `vmafx/*`, `libvmaf.c`, `log.cpp`, `log.h`, `/core/test/test_vmafx_*`, `/core/test/vmafx_test_util.h`, `/core/test/vmafx_import_test_util.h`, `/core/test/vmafx_fixture_util.h`, `compat/gcc/stdatomic.h`, `vmafx.map`, `vmafx.def`, `vmafx_symbols.txt`, `/core/api/vmafx.toml`, `/core/include/vmafx/*`, `/core/test/check_exported_symbols.py`, `/scripts/codegen/**`, `/bindings/python/vmafx/*`, `/core/tools/cli_options.gen.inc`, `/pkg/scoreopts/*`, `/proto/vmafx/v1/vmafx_api.proto`, `/api/openapi/components.gen.yaml`, `/ffmpeg-patches/src/vf_vmafx_options.h` | [vmafx-api](AGENTS.d/vmafx-api.md) | Generated from vmafx.toml, never hand-edit; engine bodies compile as vmaf_engine_*; frame ref = picture VmafRef. |
| `compat/libvmaf/*`, `vmafx/engine_names_gen.h`, `vmafx/bridge.c`, `vmafx/context_frames.c`, `conversion_context.c`, `/core/test/conversion_target_model.h`, `vmafx/convert.c`, `vmafx/tiny_model.c`, `vmafx/mcp_server.c`, `vmafx_legacy_*.map`, `/core/test/meson_cc.py`, `libvmaf_symbols.txt`, `/core/test/test_compat_*`, `/core/test/compat_conformance*` | [vmafx-compat](AGENTS.d/vmafx-compat.md) | libvmaf.so.3 links exported VMAFx symbols only; engine TUs need engine_names_gen.h; GPU exceptions declared. |
| `vmafx/device*.c`, `vmafx/fence.c`, `vmafx/frame_import*.c`, `vmafx/frame_import_hooks.h`, `vmafx/frame_pool.c`, `vmafx/sync_object.c`, `vmafx/sync_object.h`, `vmafx/release_events.c`, `vmafx/import_convert_kernels.h`, `compat/gcc/stdatomic.h`, `/core/test/test_vmafx_import_*`, `/core/test/vmafx_import_test_util.h` | [vmafx-device-frames](AGENTS.d/vmafx-device-frames.md) | Import never host-copies; release fence at last picture ref; D8 wait per context, 10 s default. |
| `vmafx/import_layout.h`, `vmafx/import_layouts_gen.h`, `vmafx/import_convert.h`, `vmafx/rgb_math.h`, `vmafx/rgb_convert.c`, `vmafx/rgb_convert.h`, `vmafx/rgb_coefficients_gen.h`, `/core/tools/yuv_input.c`, `/core/tools/y4m_input.c` | [vmafx-input-formats](AGENTS.d/vmafx-input-formats.md) | One format table, one reference per layout, RGB never guessed; the CLI reads raw layouts with the import's code. |
| `vmafx/provenance*`, `vmafx/report*`, `vmafx/exactness*`, `vmafx/build_commit.h.in`, `feature/feature_collector.cpp`, `feature/feature_collector.h`, `feature/feature_extractor.cpp`, `output.cpp`, `model.c`, `model.h`, `predict.c`, `/scripts/codegen/vmafx_exactness.py`, `/core/tools/test/test_vmaf_provenance_report.py`, `/core/tools/test/test_vmaf_verify_provenance.py` | [vmafx-provenance](AGENTS.d/vmafx-provenance.md) | Record JSON = proto JSON mapping in RFC 8785 form; digest skips digest + elapsed_ns only; field tables match vmafx.toml. |
| `vmafx/window.c`, `vmafx/window_clock.c`, `vmafx/score.c`, `predict.c`, `/core/test/test_vmafx_window*`, `/core/test/vmafx_window_test_util.h`, `/core/test/vmafx_live_pacing.h`, `/core/test/test_vmafx_live_pacing.c`, `/core/test/test_timing_suite_plain_builds.py` | [vmafx-windows](AGENTS.d/vmafx-windows.md) | Window values = sync pooled bits (vmafx_pool_engine); completion thread per context, engine lock, no fence. |
