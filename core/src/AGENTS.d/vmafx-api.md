---
paths:
  - core/src/vmafx/*
  - core/src/libvmaf.c
  - core/api/vmafx.toml
  - core/include/vmafx/*
  - scripts/codegen/**
  - bindings/python/vmafx/*
invariant: Generated from core/api/vmafx.toml, never hand-edit outputs; four libvmaf calls are shims on vmaf_engine_ bodies.
---
<!-- markdownlint-disable MD013 -->
# VMAFx API and its libvmaf compat shims (ADR-1852)

## Generated files

- Source of truth: `core/api/vmafx.toml`. Generated, committed, never hand-edited: `core/include/vmafx/*.h`, `core/src/vmafx/*_gen.{c,h}`, `core/test/test_vmafx_abi_layout.c`, `bindings/python/vmafx/_api.py`, `docs/api/vmafx/reference.md`.
- Change = edit definition, `python3 scripts/codegen/vmafx-api.py --write`. Meson `test_vmafx_api_generated_current` fails on any byte difference.
- Merge conflict in a generated file: take either side, re-run `--write`. Never hand-merge.
- Definition append-only (HISS-14): `python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master`. Break = higher ABI major + `!` + `Migration:` footer.

## Engine split in `libvmaf.c`

- `vmaf_init`, `vmaf_close`, `vmaf_version`, `vmaf_feature_score_at_index` = generated shims (`core/src/vmafx/compat_libvmaf_gen.c`) on `vmafx_*`. Their former bodies = `vmaf_engine_init`, `vmaf_engine_close`, `vmaf_engine_version`, `vmaf_engine_feature_score_at_index` (`core/src/vmafx/engine.h`, not exported).
- Upstream sync changing one of these four in `libvmaf.c`: port the change into the `vmaf_engine_*` body. Never re-add a `vmaf_init` / `vmaf_close` / `vmaf_version` / `vmaf_feature_score_at_index` definition to `libvmaf.c` (duplicate symbol with the shim).
- Engine-internal callers call `vmaf_engine_feature_score_at_index()` (pooling loop), not the shim.
- `VmafContext.api_owner`: set by `vmafx_context_create()` right after `vmaf_engine_init()`. Every engine context comes from there (also through `vmaf_init`). Compat `vmaf_close()` resolves its context through `vmafx_context_from_libvmaf()`; NULL owner = `-EINVAL`.
- `vmafx_context_destroy()` keeps the ADR-1336 retry contract: engine close failure -> status returned, context still valid.

## Errors

- Every non-OK status may carry a `VmafxError` (status, message, subject, engine errno). With `error == NULL` the message is logged at ERROR, never dropped.
- Compat shims return the engine's own negative errno when the error carries one (`compat_errno()`), else the status table's errno: libvmaf return values unchanged.
