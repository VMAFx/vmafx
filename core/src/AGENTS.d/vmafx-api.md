---
paths:
  - core/src/vmafx/*
  - core/src/libvmaf.c
  - core/src/log.cpp
  - core/src/log.h
  - core/test/test_vmafx_*
  - core/test/vmafx_test_util.h
  - core/test/vmafx_import_test_util.h
  - core/test/vmafx_fixture_util.h
  - core/src/compat/gcc/stdatomic.h
  - core/src/vmafx.map
  - core/src/vmafx.def
  - core/src/vmafx_symbols.txt
  - core/api/vmafx.toml
  - core/include/vmafx/*
  - core/test/check_exported_symbols.py
  - scripts/codegen/**
  - bindings/python/vmafx/*
  - core/tools/cli_options.gen.inc
  - pkg/scoreopts/*
  - proto/vmafx_api.proto
  - api/openapi/components.gen.yaml
  - ffmpeg-patches/src/vf_vmafx_options.h
invariant: Generated from vmafx.toml, never hand-edit; engine bodies compile as vmaf_engine_*; frame ref = picture VmafRef.
---
<!-- markdownlint-disable MD013 -->
# VMAFx API and its libvmaf compat shims (ADR-1852)

## Generated files

- Source of truth: `core/api/vmafx.toml` (`[api] schema = 2`). Generated, committed, never hand-edited: `core/include/vmafx/*.h` + `core/include/vmafx/meson.build`, `core/src/vmafx/*_gen.{c,h}`, `core/src/compat/libvmaf/*_gen.c`, `core/src/vmafx.map`, `core/src/vmafx_legacy_<backend>.map`, `core/src/vmafx.def`, `core/src/vmafx_symbols.txt`, `core/src/libvmaf_symbols.txt`, `core/test/test_vmafx_abi_layout.c`, `core/test/compat_conformance_{gen.h,table_gen.c}`, `bindings/python/vmafx/_api.py`, `docs/api/vmafx/*.md` except hand-written `index.md`.
- Change: edit definition, run `python3 scripts/codegen/vmafx-api.py --write`. Meson `test_vmafx_api_generated_current` fails on any byte difference.
- Merge conflict in generated file: take either side, re-run `--write`. Never hand-merge.
- Definition append-only (HISS-14): `python3 scripts/codegen/vmafx-api.py --abi-check --against-ref <base>`. Meson `test_vmafx_api_abi_append_only`: same check vs merge base with `origin/master`; exit 77 + reason without git, ref or base definition. Break needs higher ABI minor within 0.x, higher major from 1.0, plus `!` + `Migration:` footer; PR body lists accepted breaks.
- Addition (ADR-1897): bump `abi_version`. 0.x: `since` = current minor (patch bump) or newer, never older node. From 1.0: shipped node frozen, `since` = newer minor. Members (fields, enum values, bits) inherit parent `since` unless set.
- Schema-1 definitions (prototype #2173) read through `loader.upgrade()`, for `--abi-check` only; keep while any comparison base predates schema 2.
- One generator: missing definition feature goes into `scripts/codegen/vmafx_api/`, never worked around by hand-written output.

## Option groups (RC4 WP8, ADR-2044)

- Every scoring option of every surface = one `[[option_groups.options]]` entry. Emitters: `emit_cli` (`core/tools/cli_options.gen.inc`, included by `cli_parse.cpp` in its anon namespace), `emit_mcp` (`options.gen.json` x2: `pkg/scoreopts/`, `mcp-server/vmaf-mcp/src/vmaf_mcp/`), `emit_proto` (`proto/vmafx_api.proto`), `emit_openapi` (`api/openapi/components.gen.yaml` + splice into `vmafx-server-v1.yaml`), `emit_ffmpeg_options` (`ffmpeg-patches/src/vf_vmafx_options.h`, LGPL-2.1-or-later: compiled into FFmpeg), `emit_option_docs` (marker regions of 4 doc pages, `splice.py`).
- Library default = `default_macro` (C macro name); never a literal default model. `macros.py` reads value from `core/include/libvmaf/*.h` into `library_defaults` (drift check catches header change).
- Order = usage order + MCP `extra` argv order (parity tests pin it). New CLI spelling additive only; `core/test/test_cli_option_table.cpp` frozen list of 59 old spellings (027aebc56); master's `--check-sample-range` / `--list-backends` join definition on rebase.
- `reserved` option: every surface accepts only default (device-target size / scaling until RC5). `view_distance` range [3, 24]: default ADM CSF refuses < 3.
- Struct `proto = "Name"` -> proto message + OpenAPI schema, field number = field order (append-only struct = stable numbers); handle/ptr/callback field = generation error.
- Proto/OpenAPI regen chain: see `gen/go/AGENTS.md`. Region files: drift check compares whole file; lost marker = generation error.

## Headers, symbols, link

- `[[headers]] group`: `umbrella` (`vmafx/vmafx.h`; includes every `core` header, declares nothing), `core`, `optional` (`libvmaf_bridge.h`, later `device_<backend>.h`; never in umbrella). Includes computed from used types. Include cycle or by-value struct cycle = generation error.
- `libvmafx.so.1` links `core/src/vmafx.map` (`-Wl,--version-script`, `-Wl,--no-undefined-version`; ELF only, not darwin / windows), `hide_unlisted = true` (`local: *;`): exports vmafx_ only, plus `vmafx_legacy_<backend>.map` (node `VMAF_LEGACY_<BACKEND>`) in builds with that backend. Listed but undefined function = link error. Windows shared: `vmafx.def` not linked yet (WP12).
- `check_exported_symbols` (fast, Linux shared, two tests): libvmafx = vmafx list both ways + node per symbol + legacy exceptions of built backends; libvmaf.so.3 = `libvmaf_symbols.txt` rows for this build (`compat`, `compat:!B`, `compat:F`; `engine:B` = libvmafx), unversioned, no vmafx_. Version-definition symbols (`A VMAFX_0.1`) are not exports. `test_compat_library_gates` plants a missing row in each list.
- `*_INIT` macros set `struct_size` of nested sized structs too. Sized struct never inside unsized one; struct embedded by value never grows.

## Library split, compat layer (RC4 WP6)

- Rules: [vmafx-compat.md](vmafx-compat.md).

## Core API (RC4 WP2, ADR-1906)

- Files: `context.c` (create / destroy, log, options, queries), `register.c` (use_feature / model / set, import, resolve), `submit.c`, `score.c`, `model.c`, `frame_host.c`, `device.c` (CPU skeleton; WP3 owns `device*.c` growth), `options.c`, `sized.c`, `sha256.c`, `error.c`. Shared structs: `internal.h` (never exported).
- Every failure via `VMAFX_FAIL(report, status, errno, kind, subject, fmt, ...)`; report from `VMAFX_REPORT(context, error)`. Subject + `VmafxSubjectKind` mandatory. NULL error pointer -> delivered at ERROR ignoring level (callback, else stderr). `VMAFX_PENDING` (engine -EAGAIN) = no error, no log, output untouched.
- Full routing (ADR-1906, maintainer): engine calls wrapped `vmafx_engine_enter(context)` / `vmafx_engine_leave(previous)` (thread-local `VmafLogSink`, `log.cpp`); worker jobs: `threaded_extract_batch_func()` installs `ThreadDataBatch.log_sink` (captured at enqueue via `vmaf_log_thread_sink()`). New job type -> capture + install the sink too. Model loads: sink from `VmafxModelConfig.log_callback` (`load_begin()` / `load_end()`). Process level: set in `vmafx_context_create()` only without callback; engine init never sets it. Level / istty atomic = master PR #2207 (T-LOG-LEVEL-GLOBAL-DATA-RACE-2026-10-06); rebase onto it: take master's atomics, keep sink. No direct stdout / stderr write in `core/src`: `test_engine_log_routing_contract.py` (exception table, exact counts, shrink as WP3 / WP5 / WP6 land).
- Input structs: `vmafx_read_sized()`, minimum = introduction size, table `VMAFX_MIN_*` in `internal.h`; struct grows -> keep its entry, new input struct -> add one. Output structs: `vmafx_write_sized()` (>= 4 bytes, prefix); after an earlier size check `vmafx_store_sized()`.
- Frame ref = one count of `frame->pic.ref`. `vmafx_submit()` moves caller counts into engine (`vmafx_frame_take_picture()`), consumes on every path; same frame both inputs needs 2 refs. Last unref anywhere runs `frame_release()` (inner pool release or user release callback, device unref, free). Unref through a stack copy of the picture, never `&frame->pic`.
- Context holds `VmafxModel` / `VmafxModelSet` refs (`VmafxHeld`), dropped only after successful `vmaf_engine_close()` (ADR-1336 retry). Override only while refcount 1 (`VMAFX_E_BUSY`). Hash = SHA-256 of bytes parsed (`vmaf_model_builtin_data()` / file bytes); built-in == `sha256sum model/<file>.json`.
- `core/src/model.c` / `dict.cpp` bodies are compiled as `vmaf_engine_*` (engine names header); vmafx code may call either spelling, both reach the engine body.
- Frame colour (ADR-2094): `VmafxFrame.color` from desc; none (all UNKNOWN) -> context default. `submit.c::prepare_pair()` = check, admit, hand colour (`vmaf_engine_set_pair_colorimetry`), all before `have_frame` / `last_index` move: refused pair (`VMAFX_E_BUSY`) not counted. Context options: `perceptual_weight`, `perceptual_weight_strength`, `check_sample_range` (`context.c::parse_option()`); new key = docs of `vmafx_context_set_option` in definition too. Details: [vmafx-compat.md](vmafx-compat.md).
- Engine defect: set per-frame + pooled on same frame -> -EINVAL (T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05). ADM default CSF refuses `adm_norm_view_dist` < 3 (-EINVAL at first submit).
- Tests: `test_vmafx_{context,model,frame,score,bitexact,lifetime,sha256,log_routing}`, `test_engine_log_routing_contract.py`; bitexact needs `VMAFX_TEST_YUV_DIR` fixtures (77 without); lifetime Linux static + `--wrap=vmaf_thread_pool_destroy`, meant for `-Db_sanitize=address,undefined`.

## Device frames, fences, pools

- RC4 WP3 rules: [vmafx-device-frames.md](vmafx-device-frames.md).

## Engine split in `libvmaf.c`

- WP2 / prototype bodies renamed by hand (`vmaf_engine_init`, `_close`, `_version`, `_feature_score_at_index`, `_use_feature`, `_use_features_from_model[_collection]`, `_import_feature_score`, `_set_perceptual_weight_*`, `_feature_backend_twin`, `_registered_feature_extractor`, `_read_pictures`, `_score_at_index[_model_collection]`, `_feature_score_pooled`, `_score_pooled[_model_collection]`) = same names the engine names header gives; WP2 forwarders removed (WP6). All other libvmaf bodies keep libvmaf names in source.
- Upstream sync changing a libvmaf function body: port into the engine source as is (renamed at compile time). Never add a second definition of a libvmaf name to the engine.
- Engine-internal callers may use libvmaf names (they compile to `vmaf_engine_*`).
- `VmafContext.api_owner`: set by `vmafx_context_create()` right after `vmaf_engine_init()`. `VmafModel.api_owner` / `VmafModelCollection.api_owner` set by VMAFx model wrappers (bridge for compat handles). Compat resolves handles via `vmafx_context_from_libvmaf()` / `vmafx_model_from_libvmaf()` / `vmafx_model_set_from_libvmaf()`; NULL owner = `-EINVAL`.
- `vmafx_context_destroy()` keeps ADR-1336 retry contract: engine close failure -> status returned, context still valid.
- Picture bridge (`bridge.c`): `vmafx_frame_from_picture()` / `_to_picture()` take a new reference each; engine pictures without frame (pool, import, conversion) adopted: frame takes over priv release, restores it on last unref. Not thread-safe for one picture on two threads.

## Errors

- Every non-OK status may carry `VmafxError` (status, message, subject, engine errno). With `error == NULL` message logged at ERROR, never dropped.
- Compat shims return engine's own negative errno when error carries one (`compat_errno()`), else status table's errno: libvmaf return values unchanged.
