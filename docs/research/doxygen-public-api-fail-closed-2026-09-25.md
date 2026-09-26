<!-- markdownlint-disable MD013 MD060 -->

# Research digest: doxygen public-API warnings to zero and fail closed (2026-09-25)

Companion to **[ADR-1315](../adr/1315-doxygen-public-api-fail-closed.md)**.

## What the baseline run found

The public C API Doxygen build (`doxygen core/doc/Doxyfile.public-api`) produced
**228 warnings** at the baseline of `agent/fix-doxygen-public-api-warnings-6ba5` (clean signed base
`6ba5cb9893c27f03ca56b218db93496b7f37d1c3`), tracked by state item
`T-DOXYGEN-PUBLIC-API-WARNINGS-REGRESSED-2026-09-22`.

Distribution by header:

| Header                                    | Warnings | Primary Causes |
| ----------------------------------------- | -------: | -------------- |
| `core/include/libvmaf/pelorus/interop.h`  |      135 | 15 undocumented compounds, 120 undocumented members |
| `core/include/libvmaf/pelorus/denoise.h`  |       12 | 1 undocumented compound, 11 undocumented members |
| `core/include/libvmaf/pelorus/deband.h`   |       12 | 1 undocumented compound, 11 undocumented members |
| `core/include/libvmaf/picture_v2.h`       |       18 | 11 undocumented `VmafPicture2` members, 5 `@thread` unknown commands, 2 unresolvable `@ref` |
| `core/include/libvmaf/libvmaf_cuda.h`     |       13 | 6 `@field` commands, 4 unresolvable `@ref`, 2 `pic_params` members, multi-variable `w, h` |
| `core/include/libvmaf/libvmaf.h`          |       12 | 6 `@field` commands, 4 unresolvable `@ref`, 2 `pic_params` members, multi-variable `w, h` |
| `core/include/libvmaf/model.h`            |       12 | 5 `@field` commands, 3 unresolvable `@ref`, 4 nested `ci`/`p95`/`bootstrap` members |
| `core/include/libvmaf/dnn.h`              |       12 | 12 `@thread` unknown commands from `@thread-safety` |
| `core/include/libvmaf/libvmaf_sycl.h`     |        2 | 2 `pic_params` members, multi-variable `w, h` |
| **Total**                                 |  **228** | |

Distribution by warning category:

| Category                                                 | Count |
| -------------------------------------------------------- | ----: |
| Vendored Pelorus interop mirror headers                  |   159 |
| Missing struct member documentation in owned headers     |    22 |
| Deprecated `@field` tags in struct doc-blocks            |    17 |
| Unrecognized command `@thread` from `@thread-safety`     |    17 |
| Unresolvable cross-symbol `@ref` in struct doc-blocks    |    11 |
| Multi-variable declarations leaving earlier members bare |     2 |

## Root cause analysis

### 1. Vendored Pelorus interop mirror (`pelorus/*.h`)

In [ADR-1113](../adr/1113-vendor-pelorus-interop-abi.md), `core/include/libvmaf/pelorus/` was
vendored as an exact git tree mirror of external `VMAFx/pelorus`. `scripts/sync-pelorus-interop.sh`
enforces zero drift against upstream. The pelorus headers define C++ and C ABI types for plugin
loading, and are explicitly excluded from installation by `core/include/libvmaf/meson.build`.
Because `core/doc/Doxyfile.public-api` recursed into `core/include/libvmaf/` without an exclude
pattern for `pelorus`, it surfaced 159 warnings across code that cannot be modified locally without
breaking synchronization with external releases.

**Fix**: Add `*/pelorus/*` to `EXCLUDE_PATTERNS` in `core/doc/Doxyfile.public-api`.

### 2. Unknown `@thread` command from `@thread-safety`

Doxygen command syntax does not permit hyphens in command names. When Doxygen parses
`@thread-safety`, it splits the command name at the hyphen: it interprets `@thread` as the
command, and `-safety` as its argument. Because `@thread` is not a valid Doxygen command,
Doxygen emits `warning: Found unknown command '@thread'`. In headers carrying `@file`
(`dnn.h` and `picture_v2.h`), function doc-blocks are extracted when `EXTRACT_ALL=NO`,
causing this warning to fire on every annotated function.

**Fix**: Replace `@thread-safety <prose>` with standard Doxygen `@note Thread safety: <prose>`.

### 3. Multi-variable member declarations (`unsigned w, h;`)

In C syntax, declaring `unsigned w, h;` followed by `/**< Width and height */` attaches the
trailing Doxygen doc comment solely to `h`. Doxygen treats `w` as completely undocumented,
triggering `warning: Member w (variable) of struct ... is not documented`. This affected
`VmafPictureConfiguration` in `libvmaf.h`, `VmafCudaPictureConfiguration` in `libvmaf_cuda.h`,
`VmafSyclPictureConfiguration` in `libvmaf_sycl.h`, and `VmafPicture2` in `picture_v2.h`.

**Fix**: Split every multi-name declaration into distinct lines, e.g.:

```c
unsigned w; /**< Width in pixels */
unsigned h; /**< Height in pixels */
```

### 4. `@field` in struct doc comments

Several struct doc comments used `@field name description` inside the top-level comment
block. In Doxygen, `@field` is an unrecognized command (often confused with JSDoc or Epydoc),
so Doxygen ignores the tag and reports every struct field as undocumented.

**Fix**: Delete the `@field` list and provide per-member inline `/**< ... */` comments.

### 5. Cross-symbol `@ref` in struct doc comments

Inside a struct doc comment, Doxygen cannot resolve cross-symbol references to functions or
types declared outside the struct's translation scope (e.g. `@ref vmaf_picture_alloc`).
This emits `warning: unable to resolve reference to '...' for \ref command`.

**Fix**: Use backtick code literals (`` `vmaf_picture_alloc` ``) per ADR-0953.

### 6. Undocumented `VmafPicture2` members

`picture_v2.h` ([ADR-0928](../adr/0928-vmaf-picture-v2-explicit-backend-state.md)) added
`struct VmafPicture2` with 11 member fields, none of which had member doc comments.
Furthermore, the anonymous sub-struct `pic_params` in configuration structs had comments
on the sub-struct definition but not on the variable instance.

**Fix**: Document every field in `VmafPicture2` and provide inline comments on `pic_params`
and nested model score fields.

## Verification and fail-closed posture

1. `doxygen core/doc/Doxyfile.public-api` runs with `WARN_AS_ERROR = YES` and produces 0 warning lines.
2. `.github/workflows/doxygen-public-api.yml` sets `DOXYGEN_WARNING_CEILING: "0"`.
3. Meson `fast` suite test `core/test/test_gpu_public_header_docs.py` adds `PublicHeaderDoxygenContractTest`:
   - Verifies zero `@field` tags in `core/include/libvmaf/*.h`.
   - Verifies zero `@thread` / `@thread-safety` commands in `core/include/libvmaf/*.h`.
   - Verifies `WARN_AS_ERROR = YES` and `*/pelorus/*` exclude in `Doxyfile.public-api`.
   - Verifies `DOXYGEN_WARNING_CEILING: "0"` in `doxygen-public-api.yml`.
   - Executes Doxygen against a temporary directory to assert 0 warnings.
