# API generation

One definition, `core/api/vmafx.toml`, describes the VMAFx C API, and a
generator writes every surface from it
([ADR-1852](../adr/1852-vmafx-api-redesign.md)). The generated files are
committed; a test fails when any of them differs from what the definition
produces, so a hand edit of a generated file fails the build's tests.

## Change the API

1. Edit `core/api/vmafx.toml`.
2. Regenerate:

    ```bash
    python3 scripts/codegen/vmafx-api.py --write
    ```

3. Implement what the definition declares (hand-written C lives in
   `core/src/vmafx/`), build, and run the tests:

    ```bash
    python3 scripts/ci/run_meson_test.py -- -C build test_vmafx_abi_layout \
        test_vmafx_api_slice test_vmafx_api_generated_current test_vmafx_python_binding
    python3 -m pytest scripts/codegen/tests/test_vmafx_api_generator.py
    ```

4. Before a release, or for any change to an existing entry, check that the
   new definition is an append-only successor of the old one:

    ```bash
    python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master
    ```

The generator needs Python 3.11 or newer and nothing else (`tomllib` from the
standard library). It never runs during a normal build.

## What the definition holds

| Table | Generates | Rules the generator enforces |
| --- | --- | --- |
| `[api]` | ABI version macros, export macro | `abi_version` is `MAJOR.MINOR.PATCH` |
| `[[headers]]` | One public header each under `core/include/` | Every function names a declared header |
| `[[status]]` | `VmafxStatus` codes, `vmafx_status_name()`, the status-to-errno map of the compat layer | The first code is the OK code, values unique |
| `[[enums]]` | C enums, Python `IntEnum`s | Names and values unique |
| `[[handles]]` | Opaque types | Each names its release function |
| `[[structs]]` | Structs, `*_INIT` macros, layout asserts | `sized = true` adds `struct_size` first; fields are fixed-width scalars or strings; an enum field is `u32` with `enum = "..."` |
| `[[functions]]` | Declarations, Python methods, reference rows | Known types and pass modes; the `VmafxError **` parameter is last and the result is a status |
| `[[compat]]` | `libvmaf.h` function bodies on the new API | The target exists; a `build` field map covers every field of the struct it fills |

Parameter pass modes: `in` (value, or `const T *` for a struct), `in_const`
(`const T *` handle), `out` (`T *`), `out_handle` (`T **`), `error`
(`VmafxError **`).

## Outputs

| File | Content |
| --- | --- |
| `core/include/vmafx/vmafx.h`, `core/include/vmafx/libvmaf_bridge.h` | Public headers |
| `core/src/vmafx/status_gen.c`, `status_gen.h` | Status names and errno maps |
| `core/src/vmafx/compat_libvmaf_gen.c` | `libvmaf.h` functions implemented on the new API |
| `core/test/test_vmafx_abi_layout.c` | `_Static_assert` of every struct size, field offset and constant |
| `bindings/python/vmafx/_api.py` | ctypes binding; checks its layouts at import |
| `docs/api/vmafx/reference.md` | [Reference page](../api/vmafx/reference.md) |

Every generated C file is already in the repository's clang-format style and
the Python file in black's style, so formatter hooks never rewrite them.

## Gates

| Gate | Fails when | Shown failing by |
| --- | --- | --- |
| `test_vmafx_api_generated_current` (Meson, `fast`) | A generated file differs from the definition | `scripts/codegen/tests/test_vmafx_api_generator.py` edits a generated header and deletes the binding in a copy |
| `test_vmafx_abi_layout` (Meson, `fast`) | The compiler lays a struct out differently from the definition, or a constant changed | The Python binding's layout check refuses a planted size |
| `--abi-check` | A removed or changed function, field, constant or shim without a higher ABI major; an addition without a version bump | Six planted changes in the generator tests |
| Validation (every run) | An incomplete compat field map, an unknown type, an explicit `struct_size`, a misplaced error parameter | Four planted definitions in the generator tests |
| `check_exported_symbols` | The library exports a `vmafx_` symbol no public header declares | Existing test; accepts `vmafx_` since ADR-1852 |
