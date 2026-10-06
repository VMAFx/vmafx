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

## Headers, symbols, link

- `[[headers]] group`: `umbrella` (`vmafx/vmafx.h`; includes every `core` header, declares nothing), `core`, `optional` (`libvmaf_bridge.h`, later `device_<backend>.h`; never in umbrella). Includes computed from used types. Include cycle or by-value struct cycle = generation error.
- `libvmaf.so` links `core/src/vmafx.map` (`-Wl,--version-script`, `-Wl,--no-undefined-version`; ELF only, not darwin / windows). Listed but undefined function = link error. `hide_unlisted = false` while `vmaf_*` shares library: unlisted exports stay unversioned. Library split (ADR-1852 D3, WP6 / WP12) sets `true` (`local: *;`) and links `vmafx.def` on Windows.
- `check_exported_symbols` (fast, Linux shared): `vmafx_` exports == `core/src/vmafx_symbols.txt` both ways, version node per symbol; `vmaf_` exports vs header regex until compat lists all 107 (WP6). Version-definition symbols (`A VMAFX_0.1`) are not exports.
- `*_INIT` macros set `struct_size` of nested sized structs too. Sized struct never inside unsized one; struct embedded by value never grows.

## Core API (RC4 WP2, ADR-1906)

- Files: `context.c` (create / destroy, log, options, queries), `register.c` (use_feature / model / set, import, resolve), `submit.c`, `score.c`, `model.c`, `frame_host.c`, `device.c` (CPU skeleton; WP3 owns `device*.c` growth), `options.c`, `sized.c`, `sha256.c`, `error.c`. Shared structs: `internal.h` (never exported).
- Every failure via `VMAFX_FAIL(report, status, errno, kind, subject, fmt, ...)`; report from `VMAFX_REPORT(context, error)`. Subject + `VmafxSubjectKind` mandatory. NULL error pointer -> delivered at ERROR ignoring level (callback, else stderr). `VMAFX_PENDING` (engine -EAGAIN) = no error, no log, output untouched.
- Full routing (ADR-1906, maintainer): engine calls wrapped `vmafx_engine_enter(context)` / `vmafx_engine_leave(previous)` (thread-local `VmafLogSink`, `log.cpp`); worker jobs: `threaded_extract_batch_func()` installs `ThreadDataBatch.log_sink` (captured at enqueue via `vmaf_log_thread_sink()`). New job type -> capture + install the sink too. Model loads: sink from `VmafxModelConfig.log_callback` (`load_begin()` / `load_end()`). Process level: set in `vmafx_context_create()` only without callback; engine init never sets it. Level / istty atomic = master PR #2207 (T-LOG-LEVEL-GLOBAL-DATA-RACE-2026-10-06); rebase onto it: take master's atomics, keep sink. No direct stdout / stderr write in `core/src`: `test_engine_log_routing_contract.py` (exception table, exact counts, shrink as WP3 / WP5 / WP6 land).
- Input structs: `vmafx_read_sized()`, minimum = introduction size, table `VMAFX_MIN_*` in `internal.h`; struct grows -> keep its entry, new input struct -> add one. Output structs: `vmafx_write_sized()` (>= 4 bytes, prefix); after an earlier size check `vmafx_store_sized()`.
- Frame ref = one count of `frame->pic.ref`. `vmafx_submit()` moves caller counts into engine (`vmafx_frame_take_picture()`), consumes on every path; same frame both inputs needs 2 refs. Last unref anywhere runs `frame_release()` (inner pool release or user release callback, device unref, free). Unref through a stack copy of the picture, never `&frame->pic`.
- Context holds `VmafxModel` / `VmafxModelSet` refs (`VmafxHeld`), dropped only after successful `vmaf_engine_close()` (ADR-1336 retry). Override only while refcount 1 (`VMAFX_E_BUSY`). Hash = SHA-256 of bytes parsed (`vmaf_model_builtin_data()` / file bytes); built-in == `sha256sum model/<file>.json`.
- `core/src/model.c` / `dict.cpp` bodies called directly (`vmaf_model_feature_overload`, `vmaf_feature_dictionary_set`, ...). WP6 must rename them `vmaf_engine_*` before generating their libvmaf shims, else recursion.
- Engine defect: set per-frame + pooled on same frame -> -EINVAL (T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05). ADM default CSF refuses `adm_norm_view_dist` < 3 (-EINVAL at first submit).
- Tests: `test_vmafx_{context,model,frame,score,bitexact,lifetime,sha256,log_routing}`, `test_engine_log_routing_contract.py`; bitexact needs `VMAFX_TEST_YUV_DIR` fixtures (77 without); lifetime Linux static + `--wrap=vmaf_thread_pool_destroy`, meant for `-Db_sanitize=address,undefined`.

## Device frames, fences, pools (RC4 WP3 common lane, ADR-1929)

- Files: `device.c` (create, count, info, describe, profile), `device_context.c` (`vmafx_context_use_device`), `fence.c` (host fence objects, `vmafx_fence_*`), `frame_import.c` (import, release fence), `frame_import_admit.c` (admission, D8 helper), `frame_pool.c`, `frame_import_hooks.{c,h}` (test-only). Backend lanes add their kinds behind these functions; never a new function family per backend.
- Every `VmafxMemoryKind` / `VmafxFenceKind` declared; CPU lane: HOST memory, NONE / HOST fences. Other kinds -> `VMAFX_E_NOTSUP` naming kind until lane lands. `VmafxImportPlane` + `VmafxFence` embedded by value: frozen, never grow.
- Frame rule (ADR-1906 item 4) covers imported + pooled frames: release fence signalled where last picture count drops (`vmafx_frame_release()` / `pool_frame_release()`), any context or caller. `frame->released` = atomic `VmafxHostFence *`, set once (CAS); frame holds one ref, each handed-out fence one.
- Host fence: magic + `VmafRef` + atomic flag. Wait polls (50 us POSIX, 1 ms Windows) vs monotonic clock: shim has no `pthread_cond_timedwait`. Poll (timeout 0) unsignalled -> `VMAFX_PENDING`, no error, no log; expired timeout -> `VMAFX_E_TIMEOUT`.
- `VMAFX_IMPORT_ALLOW_COPY`: zero = zero copy (INIT zeroes all but `struct_size`). Never a host copy of device memory, flag or not.
- CPU import: planar unshifted planes bound (no copy); NV12 / P010 (>> 6) / P016 de-interleaved into one aligned `frame->owned` via `metal/iosurface_layout.h` row readers (one reference impl; change both together). Unsignalled HOST acquire -> `VMAFX_E_BUSY` (no queue on CPU).
- D8 = `vmafx_context_import_frame()`: import + `vmafx_context_admit()`; BUSY / TIMEOUT -> host wait on acquire (<= `VMAFX_IMPORT_RETRY_WAIT_NS`, 10 s) + one retry; else one failure naming input, backend, device, memory, format, bpc, size, modifiers, cause, attempts. `VmafxError` message 1023 bytes for it.
- Admission (`vmafx_admit_residency()`): host frame -> all admit; device frame -> CPU extractor refused (host copy), other backend refused, same backend = lane hook (today refused). Each refuser named. `vmafx_submit()`: both inputs same residency + device, then admission, before engine counts frame.
- `vmafx_context_use_device()`: once, before any feature / model / frame (twins picked at registration); context holds ref, dropped after successful close.
- Pool: `VmafRef` = caller + one per frame out; frame back at last count; destroy with frames out keeps them valid; exhaustion `VMAFX_E_BUSY`. Pool frames: priv + ref re-armed per acquire, cleared in release.
- C11 atomics used here (`atomic_uintptr_t`, `atomic_exchange`, CAS, `_explicit` orders) must exist in fallback `core/src/compat/gcc/stdatomic.h` too; new atomic op -> add it there.
- Test hooks (hidden symbols, static-lib tests only): counters `vmafx_count_host_copy()` (every host copy site calls it; lanes assert 0), `vmafx_count_conversion()`, import attempts; switches SKIP_ACQUIRE_WAIT, EARLY_RELEASE, FORCE_HOST_COPY; planted import status; residency override. Test that sets one clears it.
- Tests: `test_vmafx_import_api` (public), `test_vmafx_import_bitexact` (fixtures + synthetic 4K, NV12 / P010 / P016 == host frames, one import two contexts), `test_vmafx_import_fence` (Linux; acquire order, release canary incl. worker threads, host-copy counter, D8, admission). Each planted switch must make its test fail.

## Engine split in `libvmaf.c`

- `vmaf_init`, `vmaf_close`, `vmaf_version`, `vmaf_feature_score_at_index` = generated shims (`core/src/vmafx/compat_libvmaf_gen.c`) on `vmafx_*`. Former bodies = `vmaf_engine_init`, `vmaf_engine_close`, `vmaf_engine_version`, `vmaf_engine_feature_score_at_index` (`core/src/vmafx/engine.h`, not exported).
- Upstream sync changing one of these four in `libvmaf.c`: port change into `vmaf_engine_*` body. Never re-add `vmaf_init` / `vmaf_close` / `vmaf_version` / `vmaf_feature_score_at_index` definition to `libvmaf.c` (duplicate symbol with shim).
- Engine-internal callers call `vmaf_engine_feature_score_at_index()` (pooling loop), not shim.
- WP2 renamed 14 more bodies (`use_feature`, `use_features_from_model[_collection]`, `import_feature_score`, `set_perceptual_weight_*`, `feature_backend_twin`, `registered_feature_extractor`, `read_pictures`, `score_at_index[_model_collection]`, `feature_score_pooled`, `score_pooled[_model_collection]`); libvmaf names = forwarders at end of `libvmaf.c` until WP6 shims. Upstream change -> port into `vmaf_engine_*` body.
- `VmafContext.api_owner`: set by `vmafx_context_create()` right after `vmaf_engine_init()`. Every engine context comes from there (also through `vmaf_init`). Compat `vmaf_close()` resolves its context through `vmafx_context_from_libvmaf()`; NULL owner = `-EINVAL`.
- `vmafx_context_destroy()` keeps ADR-1336 retry contract: engine close failure -> status returned, context still valid.

## Errors

- Every non-OK status may carry `VmafxError` (status, message, subject, engine errno). With `error == NULL` message logged at ERROR, never dropped.
- Compat shims return engine's own negative errno when error carries one (`compat_errno()`), else status table's errno: libvmaf return values unchanged.
