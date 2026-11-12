<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src

Generated index. Pick area for touched paths (both if 2 fit); match `Touching`,
read each matching page first. Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

Scoped orientation for coding agent working directly inside `core/src/`.
Parent scope: [`../AGENTS.md`](../AGENTS.md) (core) and
[`../../AGENTS.md`](../../AGENTS.md) (root).

## Areas

| Area | Read first | Covers |
| --- | --- | --- |
| Build, inline asm, and Windows shims | [build-and-platform](AGENTS-build-and-platform.md) | meson.build, x86/avx512_warm_up.h, cpu.cpp, compat/win32/pthread*, MSVC static library names, and exported-symbol checks. |
| Engine, pictures, framesync, and I/O | [engine](AGENTS-engine.md) | libvmaf.c, picture*, gpu_picture_pool.cpp, framesync*, conversion_*, read_json_model.c, read_json_model.cpp, log*, output.cpp, thread_locale*, opt.cpp, dict.cpp, metadata_handler*, pdjson*, feature/feature_extractor.cpp, and feature/feature_name.cpp. |
| Models, prediction, pooling, and Rust | [models-and-rust](AGENTS-models-and-rust.md) | libvmaf.c, meson.build, predict*, model*, read_json_model*, svm*, percentile.h, bootstrap_names.h, interop/pelorus_interop.c, rust/**, feature/feature_extractor.*, feature/tad_rust.c, and feature/ciede* upstream-product parity. |
| VMAFx API, frames, and provenance | [vmafx](AGENTS-vmafx.md) | vmafx/*, compat/libvmaf/*, libvmaf.c, predict.c, model.c, model.h, output.cpp, log.cpp, log.h, conversion_context.c, feature/feature_collector.*, feature/feature_extractor.cpp, core/api/vmafx.toml, core/include/vmafx/*, vmafx*.map, vmafx.def, libvmaf_symbols.txt; device frame import, live windows, provenance records. |
