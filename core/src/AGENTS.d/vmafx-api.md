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
invariant: Generated from vmafx.toml, never hand-edit; engine code calls vmaf_engine_ only; frame ref = picture VmafRef.
---
<!-- markdownlint-disable MD013 -->
# VMAFx API and its libvmaf compat shims (ADR-1852)

## Generated files

- Source of truth: `core/api/vmafx.toml` (`[api] schema = 2`). Generated, committed, never hand-edited: `core/include/vmafx/*.h` + `core/include/vmafx/meson.build`, `core/src/vmafx/*_gen.{c,h}`, `core/src/vmafx.map`, `core/src/vmafx.def`, `core/src/vmafx_symbols.txt`, `core/test/test_vmafx_abi_layout.c`, `bindings/python/vmafx/_api.py`, `docs/api/vmafx/*.md` except hand-written `index.md`.
- Change: edit definition, run `python3 scripts/codegen/vmafx-api.py --write`. Meson `test_vmafx_api_generated_current` fails on any byte difference.
- Merge conflict in generated file: take either side, re-run `--write`. Never hand-merge.
- Definition append-only (HISS-14): `python3 scripts/codegen/vmafx-api.py --abi-check --against-ref <base>`. Meson `test_vmafx_api_abi_append_only`: same check vs merge base with `origin/master`; exit 77 + reason without git, ref or base definition. Break needs higher ABI minor within 0.x, higher major from 1.0, plus `!` + `Migration:` footer; PR body lists accepted breaks.
- Addition (ADR-1897): bump `abi_version`. 0.x: `since` = current minor (patch bump) or newer, never older node. From 1.0: shipped node frozen, `since` = newer minor. Members (fields, enum values, bits) inherit parent `since` unless set.
- Schema-1 definitions (prototype #2173) read through `loader.upgrade()`, for `--abi-check` only; keep while any comparison base predates schema 2.
- One generator: missing definition feature goes into `scripts/codegen/vmafx_api/`, never worked around by hand-written output.
- Generator format test (`test_vmafx_api_layout.py`): clang-format of `.pre-commit-config.yaml` pin major only (17 / 18.1.3 align `*_INIT` macros differently). `VMAFX_CLANG_FORMAT` wrong major = fail; other major on `PATH` = skip with version. Tooling Tests installs `requirements/locks/tooling-tests.in` `clang-format==<hook rev>`; bump hook rev and lock together (test asserts).

## Headers, symbols, link

- `[[headers]] group`: `umbrella` (`vmafx/vmafx.h`; includes every `core` header, declares nothing), `core`, `optional` (`libvmaf_bridge.h`, later `device_<backend>.h`; never in umbrella). Includes computed from used types. Include cycle or by-value struct cycle = generation error.
- `libvmaf.so` links `core/src/vmafx.map` (`-Wl,--version-script`, `-Wl,--no-undefined-version`; ELF only, not darwin / windows). Listed but undefined function = link error. `hide_unlisted = false` while `vmaf_*` shares library: unlisted exports stay unversioned. Library split (ADR-1852 D3, WP6 / WP12) sets `true` (`local: *;`) and links `vmafx.def` on Windows.
- `check_exported_symbols` (fast, Linux shared): `vmafx_` exports == `core/src/vmafx_symbols.txt` both ways, version node per symbol; `vmaf_` exports vs header regex until compat lists all 107 (WP6). Version-definition symbols (`A VMAFX_0.1`) = not exports.
- `*_INIT` macros set `struct_size` of nested sized structs too. Sized struct never inside unsized one; struct embedded by value never grows.

## Core API (RC4 WP2, ADR-1906)

- Files: `context.c` (create / destroy, log, options, queries), `register.c` (use_feature / model / set, import, resolve), `submit.c`, `score.c`, `model.c`, `frame_host.c`, `device.c` (CPU skeleton; WP3 owns `device*.c` growth), `options.c`, `sized.c`, `sha256.c`, `error.c`. Shared structs: `internal.h` (never exported).
- Every failure via `VMAFX_FAIL(report, status, errno, kind, subject, fmt, ...)`; report from `VMAFX_REPORT(context, error)`. Subject + `VmafxSubjectKind` mandatory. NULL error pointer -> delivered at ERROR ignoring level (callback, else stderr). `VMAFX_PENDING` (engine -EAGAIN) = no error, no log, output untouched.
- Full routing (ADR-1906, maintainer): engine calls wrapped `vmafx_engine_enter(context)` / `vmafx_engine_leave(previous)` (thread-local `VmafLogSink`, `log.cpp`); worker jobs: `threaded_extract_batch_func()` installs `ThreadDataBatch.log_sink` (captured at enqueue via `vmaf_log_thread_sink()`). New job type -> capture + install sink too. Model loads: sink from `VmafxModelConfig.log_callback` (`load_begin()` / `load_end()`). Process level: set in `vmafx_context_create()` only without callback; engine init never sets level. Level / istty atomic = master PR #2207 (T-LOG-LEVEL-GLOBAL-DATA-RACE-2026-10-06); rebase onto #2207: take master's atomics, keep sink. No direct stdout / stderr write in `core/src`: `test_engine_log_routing_contract.py` (exception table, exact counts, shrink as WP3 / WP5 / WP6 land).
- Input structs: `vmafx_read_sized()`, minimum = introduction size, table `VMAFX_MIN_*` in `internal.h`; struct grows -> keep entry, new input struct -> add entry. Output structs: `vmafx_write_sized()` (>= 4 bytes, prefix); after earlier size check `vmafx_store_sized()`.
- Frame ref = one count of `frame->pic.ref`. `vmafx_submit()` moves caller counts into engine (`vmafx_frame_take_picture()`), consumes on every path; same frame both inputs needs 2 refs. Last unref anywhere runs `frame_release()` (inner pool release or user release callback, device unref, free). Unref through stack copy of picture, never `&frame->pic`.
- Context holds `VmafxModel` / `VmafxModelSet` refs (`VmafxHeld`), dropped only after successful `vmaf_engine_close()` (ADR-1336 retry). Override only while refcount 1 (`VMAFX_E_BUSY`). Hash = SHA-256 of bytes parsed (`vmaf_model_builtin_data()` / file bytes); built-in == `sha256sum model/<file>.json` only because `.gitattributes` `model/**/*.json text eol=lf` (CRLF checkout = other hash on Windows); keep line on upstream sync, `test_praetor_hashed_files_lf.py` guards.
- `core/src/model.c` / `dict.cpp` bodies called directly (`vmaf_model_feature_overload`, `vmaf_feature_dictionary_set`, ...). WP6 renames these bodies `vmaf_engine_*` before generating libvmaf shims, else recursion.
- Engine defect: set per-frame + pooled on same frame -> -EINVAL (T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05). ADM default CSF refuses `adm_norm_view_dist` < 3 (-EINVAL at first submit).
- Tests: `test_vmafx_{context,model,frame,score,bitexact,lifetime,sha256,log_routing}`, `test_engine_log_routing_contract.py`; bitexact needs `VMAFX_TEST_YUV_DIR` fixtures (77 without); lifetime Linux static + `--wrap=vmaf_thread_pool_destroy`, meant for `-Db_sanitize=address,undefined`.

## Device frames, fences, pools

- RC4 WP3 rules: [vmafx-device-frames.md](vmafx-device-frames.md).

## Engine split in `libvmaf.c`

- `vmaf_init`, `vmaf_close`, `vmaf_version`, `vmaf_feature_score_at_index` = generated shims (`core/src/vmafx/compat_libvmaf_gen.c`) on `vmafx_*`. Former bodies = `vmaf_engine_init`, `vmaf_engine_close`, `vmaf_engine_version`, `vmaf_engine_feature_score_at_index` (`core/src/vmafx/engine.h`, not exported).
- Upstream sync changing one of these four in `libvmaf.c`: port change into `vmaf_engine_*` body. Never re-add `vmaf_init` / `vmaf_close` / `vmaf_version` / `vmaf_feature_score_at_index` definition to `libvmaf.c` (duplicate symbol with shim).
- Engine-internal callers call `vmaf_engine_feature_score_at_index()` (pooling loop), not shim.
- WP2 renamed 14 more bodies (`use_feature`, `use_features_from_model[_collection]`, `import_feature_score`, `set_perceptual_weight_*`, `feature_backend_twin`, `registered_feature_extractor`, `read_pictures`, `score_at_index[_model_collection]`, `feature_score_pooled`, `score_pooled[_model_collection]`); libvmaf names = forwarders at end of `libvmaf.c` until WP6 shims. Upstream change -> port into `vmaf_engine_*` body.
- `VmafContext.api_owner`: set by `vmafx_context_create()` right after `vmaf_engine_init()`. Every engine context comes from there (also through `vmaf_init`). Compat `vmaf_close()` resolves context through `vmafx_context_from_libvmaf()`; NULL owner = `-EINVAL`.
- `vmafx_context_destroy()` keeps ADR-1336 retry contract: engine close failure -> status returned, context still valid.

## Errors

- Every non-OK status carries optional `VmafxError` (status, message, subject, engine errno). With `error == NULL` message logged at ERROR, never dropped.
- Compat shims return engine's own negative errno when error carries one (`compat_errno()`), else status table's errno: libvmaf return values unchanged.
