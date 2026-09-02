# Research digest: doxygen public-API clean (2026-05-31)

Companion to **ADR-0953**.

## What the baseline run found

Doxygen 1.15 against `core/include/libvmaf/*.h` with the new standalone
`Doxyfile.public-api` produces **95 warnings** at HEAD of `master`.
Distribution by file:

| File                              | Warnings |
| --------------------------------- | -------: |
| `libvmaf_mcp.h`                   |       24 |
| `dnn.h`                           |       22 |
| `libvmaf_metal.h`                 |       12 |
| `picture.h`                       |        9 |
| `libvmaf_hip.h`                   |        7 |
| `libvmaf.h`                       |        7 |
| `model.h`                         |        6 |
| `libvmaf_sycl.h`                  |        4 |
| `libvmaf_cuda.h`                  |        4 |

Distribution by warning class:

| Class                                            | Count |
| ------------------------------------------------ | ----: |
| Undocumented struct members                      |    44 |
| Missing `@param` on function declarations        |    21 |
| Missing `@return` on function declarations       |    11 |
| Undocumented compound types                      |     8 |
| Unknown commands / unresolved refs / misc        |    11 |

## Root causes

1. **`@field` is not a doxygen command.** Several fork-added headers
   document struct members with `@field name desc` inside the struct's
   block comment. Doxygen treats `@field` as an unknown command, emits
   a warning, and the per-member docs disappear, which then triggers
   "Member X is not documented" on every field. Affected:
   `libvmaf_mcp.h` (8 occurrences), `dnn.h` (1), `libvmaf_metal.h` (1).
2. **Cross-symbol `@ref` from struct doc-blocks.** Doxygen resolves
   `@ref function_name` inside a function-doc block, but the same
   reference inside a struct-doc block fails to resolve and emits
   "unable to resolve reference to 'foo' for \ref command".
   Backtick literals (`vmaf_picture_alloc`) sidestep this entirely
   without losing the cross-reference intent.
3. **Function declarations carrying only a description, no
   `@param`/`@return`.** Common in the GPU-backend headers
   (`libvmaf_hip.h`, `libvmaf_metal.h`) and the MCP header
   (`libvmaf_mcp.h`); the prose described the function's behaviour
   but did not declare each parameter / the return code class.
4. **Upstream-mirrored headers without doxygen at all.** `picture.h`
   and `model.h` are Netflix-original files that ship to consumers
   but carry zero doxygen comments. The public-API generator surfaces
   every compound + member there as undocumented.

## What we did

- Added `core/doc/Doxyfile.public-api` — a standalone Doxyfile that
  targets only `core/include/libvmaf/` and turns on
  `WARN_IF_UNDOCUMENTED`, `WARN_IF_DOC_ERROR`,
  `WARN_IF_INCOMPLETE_DOC`, and `WARN_NO_PARAMDOC`. `WARN_AS_ERROR`
  stays OFF (informational-only until the gate is required).
- Converted every `@field name desc` block to per-member
  `/**< desc */` inline doc comments.
- Added missing `@param` blocks (per parameter, with the parameter
  name) and `@return` blocks (with the error-code class) to every
  affected function declaration.
- Documented the previously-undocumented compounds in `picture.h`,
  `model.h`, `libvmaf.h::VmafPictureConfiguration`,
  `libvmaf_cuda.h`, `libvmaf_hip.h`, `libvmaf_sycl.h`, and
  `libvmaf_metal.h`.
- Converted struct-doc `@ref` cross-symbol references to backtick
  literals where doxygen could not resolve them.
- Added `.github/workflows/doxygen-public-api.yml` — on-demand CI
  job that publishes the warning log + generated HTML as artifacts.

## Post-fix verification

Doxygen 1.15 run after the fixes:

```text
$ wc -l build/doxygen-public-api/warnings.log
0 build/doxygen-public-api/warnings.log
```

**95 → 0.**

## Promotion criteria

The workflow ships as informational. Promote to required-aggregator
once these conditions hold:

1. One full merge-train cycle without the public-API tree drifting
   back into a warning state.
2. `WARN_AS_ERROR=YES` toggled in `Doxyfile.public-api`.
3. The workflow added to `.github/workflows/required-aggregator.yml`'s
   wait-list.
4. A CHANGELOG entry under `changelog.d/changed/` documenting the
   gate's promotion.

## Reproducer

From the repo root (Linux, doxygen >= 1.13):

```bash
sudo apt-get install -y --no-install-recommends doxygen
mkdir -p build/doxygen-public-api
doxygen core/doc/Doxyfile.public-api
wc -l build/doxygen-public-api/warnings.log    # expect 0
open build/doxygen-public-api/html/index.html  # browse the rendered API
```
