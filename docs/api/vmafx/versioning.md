# Versioning and the ABI

The VMAFx C API provides strict Application Binary Interface (ABI) stability
guarantees. Applications compiled against an earlier ABI version remain binary
compatible with newer shared library releases without recompilation. This guide
explains compile-time and runtime version queries, symbol versioning, struct
evolution rules, the append-only policy, and deprecation mechanics.

## Checking the ABI version

The header `<vmafx/version.h>` (included by `<vmafx/vmafx.h>`) declares version
macros and runtime query functions, documented in
[`vmafx/version.h`](version.md).

### Compile-time macros

Headers define the ABI version the application compiles against:

- `VMAFX_ABI_VERSION_MAJOR`: ABI major number.
- `VMAFX_ABI_VERSION_MINOR`: ABI minor number.
- `VMAFX_ABI_VERSION_PATCH`: ABI patch number.

### Runtime version functions

To verify the ABI of the dynamically loaded library (`libvmafx.so.1`), call
`vmafx_abi_version()`:

```c
#include <stdint.h>
#include <stdio.h>
#include <vmafx/vmafx.h>

void check_version(void)
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;

    vmafx_abi_version(&major, &minor, &patch);
    if (major != VMAFX_ABI_VERSION_MAJOR ||
        minor < VMAFX_ABI_VERSION_MINOR) {
        fprintf(stderr,
                "Incompatible VMAFx ABI: loaded %u.%u.%u, "
                "compiled for %u.%u.%u\n",
                (unsigned)major, (unsigned)minor, (unsigned)patch,
                (unsigned)VMAFX_ABI_VERSION_MAJOR,
                (unsigned)VMAFX_ABI_VERSION_MINOR,
                (unsigned)VMAFX_ABI_VERSION_PATCH);
    }
}
```

Each pointer passed to `vmafx_abi_version()` may be `NULL` if that component
is not needed.

The function `vmafx_version_string()` returns a human-readable build version
string, the engine's version string. The returned pointer is valid for the
lifetime of the process.

## ABI minor numbers and `since`

The public API is defined declaratively in `core/api/vmafx.toml` (see
[API generation](../../development/api-generation.md)).

Every declaration (function, struct, field, enum, flag bit, and option) carries
a `since` attribute specifying the ABI minor that introduced it (for example,
`since = "0.1"`). Members inherit their parent container's `since` unless an
explicit later version is specified.

The generator records these versions in header comments (`@since 0.1`) and in
the [generated reference](reference.md).

## Symbol version nodes

On Linux (ELF), `libvmafx.so.1` is linked using a version script
(`core/src/vmafx.map`) generated from `core/api/vmafx.toml`.

- Each ABI minor defines a symbol version node (`VMAFX_0.1`, `VMAFX_0.2`, ...).
- Functions are exported under the version node corresponding to their `since`
  declaration.
- Inspecting the library with `nm -D libvmafx.so.1` displays versioned symbol
  bindings, such as `vmafx_context_create@@VMAFX_0.1`.

This mechanism provides load-time safety: if an application compiled against a
newer ABI minor is launched against an older shared library, the dynamic linker
fails immediately at startup with an unresolved symbol version, rather than
crashing later during execution.

Legacy `vmaf_*` functions in `libvmaf.so.3` remain unversioned for
backwards compatibility.

## Struct evolution and `struct_size`

To allow public structs to evolve without breaking binary layout, every
extensible struct declares `sized = true` in `core/api/vmafx.toml`.

- The first member of every sized struct is `uint32_t struct_size`.
- Every struct provides an initialization macro (such as
  `VMAFX_CONTEXT_CONFIG_INIT` or `VMAFX_FRAME_DESC_INIT`) that sets
  `struct_size = sizeof(...)`. Always initialize structs with their macro.
- Sized structs grow strictly by appending new fields to the end.

### Backward and forward compatibility rules

When passing structs across the ABI boundary:

1. **Older caller, newer library**: The library reads only the fields included
   in the caller's `struct_size`. Any fields added in newer versions receive
   their documented default values.
2. **Invalid size (`VMAFX_E_ABI`)**: If an input struct has `struct_size`
   smaller than the size of the struct when it was introduced, the call fails
   with `VMAFX_E_ABI` (status `-11`, defined in
   [`vmafx/types.h`](types.md#status-codes)). The error subject is set to the
   name of the invalid struct.
3. **Output structs**: An output struct receives data only up to its provided
   `struct_size`. On return, the library sets `struct_size` to the number of
   bytes actually written.
4. **Embedded structs**: Structs embedded by value inside another struct never
   grow and do not carry a `struct_size` field.

## Append-only policy

The API follows the HISS-14 append-only invariant (`AGENTS.md`) and
[ADR-1852](../../adr/1852-vmafx-api-redesign.md):

- Within an ABI major version, changes to the API must be purely additive.
- Allowed additions include new functions, fields appended to sized structs,
  new enum constants, new flag bits, and new configuration options.
- Breaking changes are strictly prohibited: removing or renaming declarations,
  modifying function parameter lists or callback signatures, reordering,
  retyping, or resizing existing fields, renumbering constants or bits,
  altering `since`, or extending embedded-by-value structs.

### Pre-1.0 vs Post-1.0 policy

- **During 0.x**: Breaking changes require advancing the ABI minor (for
  example, `0.1.0` to `0.2.0`), with all breaks documented in the pull
  request.
- **From 1.0.0 onward**: Breaking changes require an ABI major bump, an
  exclamation mark (`!`) in the pull request title, and a `Migration:` footer.

Compliance is validated automatically using the generator's ABI checker:

```bash
python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master
```

## Option deprecation

Options and API entries in `core/api/vmafx.toml` may declare deprecations with
three fields:

```toml
deprecated = { since = "0.2", replacement = "new_option", removal = "1.0" }
```

- `since`: The ABI minor in which the element was deprecated.
- `replacement`: Name of the replacement function, type, field (`Struct.field`),
  or option.
- `removal`: Target ABI version where the deprecated element will be dropped.

The generator applies the `VMAFX_DEPRECATED("...")` attribute to deprecated
declarations in generated C headers. Callers can suppress compiler warnings by
defining `VMAFX_NO_DEPRECATION_WARNINGS`.

### Changelog generator

The script `scripts/codegen/vmafx_api/changelog.py` detects deprecations and
additions between two definitions:

```bash
python3 scripts/codegen/vmafx-api.py --changelog origin/master
```

It formats draft changelog fragments for `changelog.d/changed/api-<slug>.md`:

```markdown
- **`entry` is deprecated** since VMAFx API 0.2: use `replacement`; it
  is removed in 1.0.
```

## See also

- [Errors](errors.md) for status codes and error handling
- [vmafx/version.h](version.md) for version function declarations
- [vmafx/types.h](types.md) for `VmafxStatus` and status code definitions
- [API generation](../../development/api-generation.md) for generator workflows
- [ADR-1852](../../adr/1852-vmafx-api-redesign.md) for API redesign decisions
