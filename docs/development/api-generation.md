# API generation

One definition, `core/api/vmafx.toml`, describes the VMAFx C API, and a
generator writes every surface from it
([ADR-1852](../adr/1852-vmafx-api-redesign.md)): the public headers, the
linker version script and export lists, the ABI layout test, the libvmaf
compatibility shims, the Python binding and the reference pages. The generated
files are committed; a test fails when any of them differs from what the
definition produces, so a hand edit of a generated file fails the build's
tests.

## Change the API

1. Edit `core/api/vmafx.toml`. A new entry names its header (`header`) and the
   ABI minor that introduces it (`since`); raise `[api] abi_version` (see
   [ABI rules](#abi-rules) for which part).
2. Regenerate:

    ```bash
    python3 scripts/codegen/vmafx-api.py --write
    ```

3. Implement what the definition declares (hand-written C lives in
   `core/src/vmafx/`; [ADR-1906](../adr/1906-vmafx-core-api-semantics.md)
   records the rules it follows for logging, struct sizes and references),
   build, and run the tests:

    ```bash
    python3 scripts/ci/run_meson_test.py -- -C build test_vmafx_abi_layout \
        test_vmafx_api_slice test_vmafx_api_generated_current \
        test_vmafx_api_abi_append_only test_vmafx_api_generator \
        test_vmafx_python_binding check_exported_symbols \
        test_vmafx_context test_vmafx_model test_vmafx_frame test_vmafx_score \
        test_vmafx_bitexact test_vmafx_lifetime test_vmafx_sha256
    ```

4. Check that the new definition is an append-only successor of the one you
   started from, and draft the changelog fragment:

    ```bash
    python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master
    python3 scripts/codegen/vmafx-api.py --changelog origin/master
    ```

    `--changelog` prints the text of `changelog.d/added/api-<slug>.md` (the
    symbols, types, fields, constants and options the definition gained) and
    of `changelog.d/changed/api-<slug>.md` (entries newly deprecated); pick
    the slug, edit the wording and commit the fragments yourself.

The generator needs Python 3.11 or newer and nothing else (`tomllib` from the
standard library). It never runs during a normal build.

## ABI rules

- **Append-only within an ABI major** (HISS-14): new functions, new fields at
  the end of a struct with `sized = true`, new constants, new flag bits and
  new options. Removing or renaming anything, changing a parameter list or a
  callback signature, reordering, retyping or resizing a field, renumbering a
  constant or a bit, changing `since`, changing an option's type or proto field
  number, or growing a struct that another struct embeds by value is a break.
- **Before `v1.0.0`** the ABI is 0.x (ADR-1852 decision D2): a break needs a
  higher minor (`0.1.0` to `0.2.0`), and the pull request lists every break
  `--abi-check` reports as accepted. From `1.0.0` a break needs a higher major,
  `!` in the pull request title and a `Migration:` footer.
- **Additions** raise `abi_version`. A function's `since` selects its linker
  version node (`VMAFX_0.1`, `VMAFX_0.2`, ...). Within 0.x an addition may join
  the current minor's node (a patch bump such as `0.1.0` to `0.1.1` is
  enough), so work packages that add functions in parallel do not each claim
  a minor; it may never join an older node. From `1.0.0` a node that shipped
  is frozen: an addition's `since` is a newer minor than the ABI it is
  compared with. [ADR-1897](../adr/1897-vmafx-abi-0x-numbering.md) records
  this rule and the alternative of a new minor per addition.
- **Deprecation**: `deprecated = { since, replacement, removal }` adds the
  `VMAFX_DEPRECATED("...")` attribute to a function (define
  `VMAFX_NO_DEPRECATION_WARNINGS` to silence it), an `@deprecated` line to the
  header documentation and a note to the reference page. `replacement` must
  name a declared function, type, constant, field (`Struct.field`) or option.

## What the definition holds

| Table | Generates | Rules the generator enforces |
| --- | --- | --- |
| `[api]` | ABI version macros (`version_header`), export and deprecation macros and status codes (`base_header`), `hide_unlisted` for the version script | `schema = 2`; `abi_version` is `MAJOR.MINOR.PATCH`; both named headers are `core` headers |
| `[[headers]]` | One public header each under `core/include/`, its install line and its reference page | `group` is `umbrella` (exactly one; includes every `core` header and declares nothing), `core`, or `optional` (never included by the umbrella; `includes` names outside headers it needs) |
| `[[status]]` | `VmafxStatus` codes, `vmafx_status_name()`, the status-to-errno map of the compat layer | The first code is the OK code; names and values unique |
| `[[enums]]` | C enums, Python `IntEnum`s | Names and values unique; fields carry them as `u32` with `enum = "..."` |
| `[[flags]]` | `#define NAME (UINT32_C(1) << bitU)` constants, Python `IntFlag`s | `type` is `u32` or `u64`; bits unique and inside the width; fields carry them with `flags = "..."` |
| `[[handles]]` | Opaque types | Each names its declared release function |
| `[[callbacks]]` | Function-pointer typedefs | Parameters are inputs; the last one is `user` of type `ptr` (`void *user`) |
| `[[structs]]` | Structs, `*_INIT` macros, layout asserts | `sized = true` adds `struct_size` first; fields are fixed-width scalars, `ptr`, `cstr`, `size`, `uptr`, nested structs, handles or callbacks; `count = N` makes a fixed array (1 to 64); `const = true` on a handle field; a sized struct is never embedded in an unsized one; no by-value cycle |
| `[[functions]]` | Declarations, version-script and export lines, Python signatures, reference rows | Known types and pass modes; the `VmafxError **` parameter is last and the result is a status |
| `[[compat]]` | `libvmaf.h` function bodies on the new API | The target exists; a `build` field map covers every field of the struct it fills |
| `[[option_groups]]` | Nothing yet: the CLI, FFmpeg, MCP and proto surfaces are emitted from them in a later work package | `surfaces` is a subset of `cli`, `ffmpeg`, `mcp`, `proto`; every option has a spelling for each, a default inside its `range` or `enum`, and spellings unique per surface |

Every top-level entry carries `header` (except status codes and option groups)
and `since`. Enum values, flag bits, struct fields and options inherit their
parent's `since` unless they name a later one. Each header includes the
headers whose types its declarations use, computed from the definition; an
include cycle stops generation.

Parameter pass modes: `in` (value; `const T *` for a struct; `T *` for a
handle), `in_const` (`const T *`), `out` (`T *`), `out_handle` (`T **`),
`error` (`VmafxError **`). Scalar types: `u32`, `u64`, `i32`, `i64`, `f32`,
`f64`, `uptr` (`uintptr_t`), `size` (`size_t`), `ptr` (`void *`), `cstr`
(`const char *`), `status`.

[`scripts/codegen/tests/fixtures/full.toml`](../../scripts/codegen/tests/fixtures/full.toml)
uses every feature of the format; the generator tests render it and compile
its C output.

## Outputs

| File | Content |
| --- | --- |
| `core/include/vmafx/*.h` | Public headers: the umbrella `vmafx.h`, the core headers of the design's header layout, and optional headers such as `libvmaf_bridge.h` |
| `core/include/vmafx/meson.build` | Install list of those headers |
| `core/src/vmafx.map` | Linker version script, one node per ABI minor; libvmaf links it on ELF targets |
| `core/src/vmafx.def` | Windows export list, for the shared library split |
| `core/src/vmafx_symbols.txt` | Exported symbols and their version nodes, read by `check_exported_symbols` |
| `core/src/vmafx/status_gen.c`, `status_gen.h` | Status names and errno maps |
| `core/src/vmafx/compat_libvmaf_gen.c` | `libvmaf.h` functions implemented on the new API |
| `core/test/test_vmafx_abi_layout.c` | `_Static_assert` of every struct size, alignment, field offset, array length and constant |
| `bindings/python/vmafx/_api.py` | ctypes binding; checks its layouts at import |
| `docs/api/vmafx/reference.md` and one page per header | [Reference index](../api/vmafx/reference.md) |

Every generated C file is already in the repository's clang-format style (long
`*_INIT` macros sit between `clang-format off` / `on` markers) and the Python
file in black's style, so formatter hooks never rewrite them.

While the VMAFx functions share `libvmaf.so` with the libvmaf API,
`hide_unlisted = false` keeps the version script from touching the `vmaf_*`
exports: they stay unversioned and only `vmafx_*` symbols get version nodes.
The library split (ADR-1852 decision D3) sets it to `true`, which hides every
symbol the script does not list.

## Gates

| Gate | Fails when | Shown failing by |
| --- | --- | --- |
| `test_vmafx_api_generated_current` (Meson, `fast`) | A generated file differs from the definition | The generator tests edit a generated header, delete the binding and a reference page, and rewrite the version script in a copy |
| `test_vmafx_abi_layout` (Meson, `fast`) | The compiler lays a struct out differently from the definition, an array has another length, or a constant changed | The generator tests compile the fixture's layout test, then a copy with `plane[3]` made `plane[4]` and one with a field widened |
| `test_vmafx_api_abi_append_only` (Meson, `fast`) | The definition is not an append-only successor of the one at the merge base with `origin/master`; skipped (exit 77, reason printed) without git, without that ref, or when the merge base has no definition | The generator tests run it in a scratch repository with a renumbered constant |
| `--abi-check` | A break without the version bump the ABI rules ask for; an addition in a version node that shipped; an addition without a version bump | Planted changes in `test_vmafx_api_generator.py` and `test_vmafx_api_abi_features.py` |
| `check_exported_symbols` (Meson, `fast`) | The library exports a `vmafx_` symbol missing from `vmafx_symbols.txt`, does not export a listed one, or exports one in another version node | `test_vmafx_api_symbols.py` builds a small library and plants each defect |
| Link of `libvmaf.so` | The version script names a function no source defines (`--no-undefined-version`) | `test_vmafx_api_symbols.py` links without one listed function |
| Validation (every run) | Any rule in the table above | Planted definitions in the generator tests |
| `test_vmafx_api_generator` (Meson, `fast`) | Any of the generator tests fails | Runs `scripts/codegen/tests/test_*.py` |

The compiler, `clang-format` and linker cases of the generator tests skip,
with the reason, when the tool is not installed.
